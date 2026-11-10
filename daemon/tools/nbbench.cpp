// nbbench: how fast each nbd stage runs on this machine, on synthetic IQ
// (768 kS/s, CW beacon + one USB station + noise). Run it on the board:
// above 1x the stage keeps up with the live stream; the total must stay
// well above 1x for nbd to be usable.
//   nbbench [--seconds 10] [--wisdom DIR] [--dsp-size 2048] [--legacy]
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "beacon_lock.h"
#include "opus_audio.h"
#include "spectrum.h"
#include "tx_dsp.h"
#include "wdsp_rx.h"
#include "wdsp_tx.h"

namespace {

using clk = std::chrono::steady_clock;
constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 768000.0;
constexpr int kBlock = 4096;

double since(clk::time_point t) { return std::chrono::duration<double>(clk::now() - t).count(); }

void report(const char* name, double wall, double secs) {
    std::printf("%-34s %7.3f s  %7.2fx real time  (%5.1f%% of one core)\n", name, wall, secs / wall,
                100.0 * wall / secs);
}

}  // namespace

int main(int argc, char** argv) {
    double seconds = 10.0;
    std::string wisdom;
    int dsp_size = 2048;
    bool legacy = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (a == "--wisdom" && i + 1 < argc) wisdom = argv[++i];
        else if (a == "--dsp-size" && i + 1 < argc) dsp_size = std::atoi(argv[++i]);
        else if (a == "--legacy") legacy = true;
        else {
            std::fprintf(stderr, "usage: nbbench [--seconds S] [--wisdom DIR] [--dsp-size N] [--legacy]\n");
            return 2;
        }
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep the lines if a stage aborts
    const size_t blocks = static_cast<size_t>(seconds * kRate / kBlock);
    const double secs = blocks * kBlock / kRate;

    std::printf("generating %.1f s of IQ at 768 kS/s...\n", secs);
    std::vector<nb::cf> iq(blocks * kBlock);
    std::mt19937 rng(1);
    std::normal_distribution<float> noise(0.0f, 0.02f);
    const double beacon = -250000.0 + 2500.0, station = 150000.0 + 2500.0 + 1000.0;
    for (size_t n = 0; n < iq.size(); ++n) {
        const double t = n / kRate;
        iq[n] = nb::cf(0.2f * static_cast<float>(std::cos(2 * kPi * beacon * t)) + 0.07f * static_cast<float>(std::cos(2 * kPi * station * t)) + noise(rng),
                       0.2f * static_cast<float>(std::sin(2 * kPi * beacon * t)) + 0.07f * static_cast<float>(std::sin(2 * kPi * station * t)) + noise(rng));
    }
    if (!wisdom.empty()) {
        std::printf("FFTW plans (slow the first time)...\n");
        nb::wdsp_wisdom(wisdom.c_str());
    }

    {
        nb::BeaconLockConfig c;
        nb::BeaconLock lock(c);
        auto t = clk::now();
        size_t b = 0;
        for (; b < blocks && lock.state() == nb::LockState::Searching; ++b) lock.process(&iq[b * kBlock], kBlock);
        const double search_wall = since(t);
        report("beacon lock, searching", search_wall, b * kBlock / kRate);
        t = clk::now();
        const size_t first = b;
        for (; b < blocks; ++b) lock.process(&iq[b * kBlock], kBlock);
        if (b > first) report("beacon lock, tracking", since(t), (b - first) * kBlock / kRate);
        std::printf("  beacon %s, correction %+.1f Hz (expected +2500)\n", nb::to_string(lock.status().state),
                    lock.status().correction_hz);
    }

    std::vector<std::vector<nb::cf>> narrow(blocks);
    {
        nb::Downconverter fe(kRate, 16, 6500.0);
        fe.set_frequency(station - 1000.0);
        auto t = clk::now();
        for (size_t b = 0; b < blocks; ++b) fe.process(&iq[b * kBlock], kBlock, narrow[b]);
        report("front end 768k -> 48k", since(t), secs);
    }
    {
        // Waterfall computed on the CPU (fallback when Maia's FPGA one is
        // not available), 8 lines/s, packed for a 1024-pixel screen.
        nb::SpectrumAnalyzer an(kRate);
        an.set_line_rate(8);
        nb::SpectrumView view(10489750000.0, kRate);
        nb::SpectrumRequest r;
        r.bins = 1024;
        view.set(r);
        std::vector<float> line;
        size_t packed = 0;
        auto t = clk::now();
        for (size_t b = 0; b < blocks; ++b) {
            an.process(&iq[b * kBlock], kBlock);
            if (an.take_line(line)) packed += view.pack(line, 0.0, 0).size();
        }
        report("waterfall, local FFT 8 lines/s", since(t), secs);
        if (packed == 0) std::printf("  (no line produced)\n");
    }

    std::vector<float> audio, all_audio;
    {
        auto t = clk::now();
        nb::WdspRx rx(0, 48000, kBlock / 16, dsp_size);
        const double open_wall = since(t);
        t = clk::now();
        for (size_t b = 0; b < blocks; ++b) {
            audio.clear();
            rx.process(narrow[b].data(), audio);
            all_audio.insert(all_audio.end(), audio.begin(), audio.end());
        }
        char name[64];
        std::snprintf(name, sizeof name, "WDSP USB at 48k (dsp %d)", dsp_size);
        report(name, since(t), secs);
        std::printf("  channel opened in %.2f s, S %.1f dB\n", open_wall, rx.signal_db());
    }
    if (legacy) {
        nb::WdspRx rx(1, static_cast<int>(kRate), kBlock, 4096);
        rx.set_offset_hz(station - 1000.0);
        auto t = clk::now();
        for (size_t b = 0; b < blocks; ++b) {
            audio.clear();
            rx.process(&iq[b * kBlock], audio);
        }
        report("WDSP USB at 768k (old path)", since(t), secs);
    }
    {
        size_t bad = 0;
        float peak = 0.0f;
        for (float v : all_audio) {
            if (!std::isfinite(v)) ++bad;
            else peak = std::max(peak, std::fabs(v));
        }
        std::printf("  audio: %zu samples, peak %.3f, non-finite %zu\n", all_audio.size(), peak, bad);
        nb::OpusAudio tone_enc(16000, 5);
        std::vector<float> tone(48000);
        for (size_t i = 0; i < tone.size(); ++i) tone[i] = 0.3f * static_cast<float>(std::sin(2 * kPi * 1000.0 * i / 48000.0));
        size_t tone_packets = 0;
        tone_enc.push(tone.data(), tone.size(), [&](const std::string&) { ++tone_packets; });
        std::printf("  Opus on a clean 1 kHz tone: %zu packets (expected 50)\n", tone_packets);
        nb::OpusAudio enc(16000, 5);
        size_t packets = 0;
        auto t = clk::now();
        enc.push(all_audio.data(), all_audio.size(), [&](const std::string&) { ++packets; });
        report("Opus 16 kbit/s, complexity 5", since(t), all_audio.size() / 48000.0);
        std::printf("  %zu packets, %llu non-finite samples replaced\n", packets,
                    static_cast<unsigned long long>(enc.bad_samples()));
    }
    {
        // Transmit path, on top of reception while transmitting (full duplex).
        std::vector<float> mic(48000 * static_cast<size_t>(secs));
        for (size_t i = 0; i < mic.size(); ++i) mic[i] = 0.3f * static_cast<float>(std::sin(2 * kPi * 900.0 * i / 48000.0));
        nb::OpusAudio menc(24000, 5);
        std::vector<std::string> packets;
        menc.push(mic.data(), mic.size(), [&](const std::string& p) { packets.push_back(p); });
        nb::OpusMicDecoder dec;
        std::vector<float> pcm;
        auto t = clk::now();
        for (const auto& p : packets) dec.decode(p, pcm);
        report("TX: Opus decode (microphone)", since(t), secs);
        pcm.resize(mic.size());
        nb::WdspTx tx(2, 512, 1024);
        std::vector<nb::cf> base, all_base;
        t = clk::now();
        for (size_t b = 0; b + 512 <= pcm.size(); b += 512) {
            tx.process(&pcm[b], base);
            all_base.insert(all_base.end(), base.begin(), base.end());
        }
        report("TX: WDSP USB modulator at 48k", since(t), secs);
        nb::Upconverter up(3072000.0);
        up.set_offset_hz(800000.0);
        std::vector<int16_t> iq16;
        t = clk::now();
        for (size_t b = 0; b + 512 <= all_base.size(); b += 512) {
            iq16.clear();
            up.process(&all_base[b], 512, iq16);
        }
        report("TX: upconverter to 3.072 MS/s", since(t), all_base.size() / 48000.0);
    }
    std::printf("total of the tracking path must stay well above 1x\n");
    return 0;
}
