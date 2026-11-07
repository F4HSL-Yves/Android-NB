// nbd: QO-100 narrowband receiver core.
//
// First version: reads the IQ stream of the narrowband transponder (from a
// file, or from stdin so it can sit behind iio_readdev on the board), locks
// on the CW beacon, demodulates one USB station with WDSP and writes 48 kHz
// mono audio. The network side (Opus, WebSocket, MQTT) comes next.
//
// Example on the board, DDC already set to 768 kS/s around 10489.750 MHz
// (the whole 500 kHz transponder plus the CW beacon):
//   iio_readdev -u local: -b 4096 cf-ad9361-lpc voltage0 voltage1 |
//     nbd --center 10489750000 --freq 10489900000 --output - > /dev/null
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

#include "beacon_lock.h"
#include "wdsp_rx.h"

namespace {

struct Options {
    std::string input = "-";
    std::string output;
    std::string format = "cs16";  // cs16 (12-bit samples in int16) or cf32
    double rate = 768000.0;
    double center_hz = 10489750000.0;  // downlink frequency at the stream centre
    double freq_hz = 10489900000.0;    // station to demodulate (USB dial)
    double beacon_hz = 10489500000.0;  // CW beacon
    double search_hz = 25000.0;
    std::string wisdom_dir;
    bool quiet = false;
};

void usage() {
    std::fprintf(stderr,
                 "usage: nbd [options]\n"
                 "  --input FILE     IQ input, '-' for stdin (default -)\n"
                 "  --format F       cs16 (default, 12-bit in int16) or cf32\n"
                 "  --rate HZ        IQ sample rate (default 768000)\n"
                 "  --center HZ      downlink frequency at the stream centre (default 10489750000)\n"
                 "  --freq HZ        USB dial frequency to demodulate (default 10489900000)\n"
                 "  --beacon HZ      CW beacon frequency (default 10489500000)\n"
                 "  --search HZ      beacon search half width (default 25000)\n"
                 "  --output FILE    48 kHz mono s16le audio, '-' for stdout\n"
                 "  --wisdom DIR     cache FFTW plans in DIR (recommended on the board)\n"
                 "  --quiet          no status lines on stderr\n");
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "nbd: %s needs a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--input") o.input = next("--input");
        else if (a == "--output") o.output = next("--output");
        else if (a == "--format") o.format = next("--format");
        else if (a == "--rate") o.rate = std::atof(next("--rate"));
        else if (a == "--center") o.center_hz = std::atof(next("--center"));
        else if (a == "--freq") o.freq_hz = std::atof(next("--freq"));
        else if (a == "--beacon") o.beacon_hz = std::atof(next("--beacon"));
        else if (a == "--search") o.search_hz = std::atof(next("--search"));
        else if (a == "--wisdom") o.wisdom_dir = next("--wisdom");
        else if (a == "--quiet") o.quiet = true;
        else if (a == "--help" || a == "-h") { usage(); std::exit(0); }
        else {
            std::fprintf(stderr, "nbd: unknown option %s\n", a.c_str());
            usage();
            return false;
        }
    }
    if (o.format != "cs16" && o.format != "cf32") {
        std::fprintf(stderr, "nbd: --format must be cs16 or cf32\n");
        return false;
    }
    if (o.rate <= 0 || std::fmod(o.rate, 375.0) != 0.0) {
        // 4096-sample blocks must map to a whole number of 48 kHz samples.
        std::fprintf(stderr, "nbd: --rate must be a multiple of 375 Hz (768000 recommended)\n");
        return false;
    }
    return true;
}

// Reads exactly n complex samples; returns false at end of input.
bool read_block(FILE* f, const std::string& format, std::vector<nb::cf>& out, size_t n) {
    out.resize(n);
    if (format == "cf32") return std::fread(out.data(), sizeof(nb::cf), n, f) == n;
    static std::vector<int16_t> raw;
    raw.resize(2 * n);
    if (std::fread(raw.data(), sizeof(int16_t), 2 * n, f) != 2 * n) return false;
    constexpr float k = 1.0f / 2048.0f;
    for (size_t i = 0; i < n; ++i) out[i] = nb::cf(raw[2 * i] * k, raw[2 * i + 1] * k);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, o)) return 2;

    FILE* in = o.input == "-" ? stdin : std::fopen(o.input.c_str(), "rb");
    if (!in) {
        std::fprintf(stderr, "nbd: cannot open %s: %s\n", o.input.c_str(), std::strerror(errno));
        return 1;
    }
    FILE* out = nullptr;
    if (!o.output.empty()) {
        out = o.output == "-" ? stdout : std::fopen(o.output.c_str(), "wb");
        if (!out) {
            std::fprintf(stderr, "nbd: cannot open %s: %s\n", o.output.c_str(), std::strerror(errno));
            return 1;
        }
    }
    if (!o.wisdom_dir.empty()) nb::wdsp_wisdom(o.wisdom_dir.c_str());

    nb::BeaconLockConfig cfg;
    cfg.sample_rate = o.rate;
    cfg.beacon_offset_hz = o.beacon_hz - o.center_hz;
    cfg.search_half_width_hz = o.search_hz;
    if (std::abs(cfg.beacon_offset_hz) + cfg.search_half_width_hz > o.rate / 2) {
        std::fprintf(stderr, "nbd: the beacon search window falls outside the IQ bandwidth\n");
        return 2;
    }
    nb::BeaconLock lock(cfg);

    const int block = 4096;
    nb::WdspRx rx(0, static_cast<int>(o.rate), block);
    const double station_offset = o.freq_hz - o.center_hz;

    const auto wall_start = std::chrono::steady_clock::now();
    std::vector<nb::cf> iq;
    std::vector<float> audio;
    std::vector<int16_t> pcm;
    uint64_t total = 0;
    double next_report = 1.0;
    while (read_block(in, o.format, iq, block)) {
        lock.process(iq.data(), iq.size());
        // Before the first lock the correction is 0 (nominal LNB); after a
        // loss it keeps the last estimate.
        rx.set_offset_hz(station_offset + lock.correction_hz());
        audio.clear();
        rx.process(iq.data(), audio);
        if (out) {
            pcm.resize(audio.size());
            for (size_t i = 0; i < audio.size(); ++i) {
                const float s = std::fmax(-1.0f, std::fmin(1.0f, audio[i]));
                pcm[i] = static_cast<int16_t>(std::lrint(s * 32767.0f));
            }
            std::fwrite(pcm.data(), sizeof(int16_t), pcm.size(), out);
        }
        total += iq.size();
        const double t = total / o.rate;
        if (!o.quiet && t >= next_report) {
            const auto st = lock.status();
            std::fprintf(stderr, "t=%7.1f s  beacon %-9s  correction %+10.1f Hz  drift %+6.2f Hz/s  snr %5.1f dB  S %6.1f dB\n",
                         t, nb::to_string(st.state), st.correction_hz, st.drift_hz_per_s, st.acquisition_snr_db,
                         rx.signal_db());
            next_report += 1.0;
        }
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
    if (!o.quiet && wall > 0.0) {
        // On a file this measures the CPU margin: above 1x, the board keeps up.
        std::fprintf(stderr, "nbd: %.1f s of IQ in %.1f s, %.2fx real time\n", total / o.rate, wall,
                     total / o.rate / wall);
    }
    if (out && out != stdout) std::fclose(out);
    if (in != stdin) std::fclose(in);
    return 0;
}
