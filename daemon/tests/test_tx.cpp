// Transmit chain: CIC / upconverter spectra, WDSP TX sideband, microphone
// jitter buffer, protocol, and the Transmitter end to end on a paced fake
// DAC (Opus packets in, IQ out at the expected uplink offset, safety stops).
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dsp.h"
#include "opus_audio.h"
#include "pluto_tx.h"
#include "protocol.h"
#include "transmitter.h"
#include "tx_dsp.h"
#include "wdsp_tx.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
int failures = 0;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                          \
            std::printf("\n");                                 \
            ++failures;                                        \
        }                                                      \
    } while (0)

// Power spectrum (dB, relative to the strongest bin) of complex samples,
// Blackman-Harris 4-term window; returns bin frequencies in Hz via `df`.
std::vector<double> spectrum_db(const std::vector<std::complex<double>>& x, double fs, double& df) {
    const size_t n = x.size();
    std::vector<std::complex<double>> a(n);
    for (size_t i = 0; i < n; ++i) {
        const double t = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n);
        const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
        a[i] = x[i] * w;
    }
    nb::fft_inplace(a);
    std::vector<double> p(n);
    double peak = 1e-300;
    for (size_t i = 0; i < n; ++i) {
        p[i] = std::norm(a[i]);
        peak = std::max(peak, p[i]);
    }
    for (auto& v : p) v = 10.0 * std::log10(v / peak + 1e-30);
    df = fs / static_cast<double>(n);
    return p;
}

// Value at frequency f (Hz, may be negative): the strongest bin within +/- span.
double level_at(const std::vector<double>& p, double df, double f, double span) {
    const long n = static_cast<long>(p.size());
    const long c = std::lround(f / df);
    const long w = std::max(1L, std::lround(span / df));
    double best = -400;
    for (long k = c - w; k <= c + w; ++k) best = std::max(best, p[static_cast<size_t>(((k % n) + n) % n)]);
    return best;
}

// Frequency of the strongest bin, Hz in [-fs/2, fs/2).
double peak_freq(const std::vector<double>& p, double df) {
    size_t best = 0;
    for (size_t i = 1; i < p.size(); ++i)
        if (p[i] > p[best]) best = i;
    const long n = static_cast<long>(p.size());
    long k = static_cast<long>(best);
    if (k >= n / 2) k -= n;
    return static_cast<double>(k) * df;
}

std::vector<std::complex<double>> to_complex(const std::vector<int16_t>& iq, size_t skip, size_t n) {
    std::vector<std::complex<double>> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = {static_cast<double>(iq[2 * (skip + i)]), static_cast<double>(iq[2 * (skip + i) + 1])};
    return x;
}

// ---------------------------------------------------------------- tests

void test_upconverter() {
    std::printf("upconverter: CIC images, NCO offset, level\n");
    const double fs = 3072000.0;
    nb::Upconverter up(fs, 0.8);
    CHECK(up.ratio() == 64, "ratio %u", up.ratio());
    std::vector<nb::cf> in(48000 / 4);
    for (size_t i = 0; i < in.size(); ++i) {
        const double ph = 2.0 * kPi * 1500.0 * static_cast<double>(i) / 48000.0;
        in[i] = nb::cf(static_cast<float>(0.9 * std::cos(ph)), static_cast<float>(0.9 * std::sin(ph)));
    }
    std::vector<int16_t> out;
    up.process(in.data(), in.size(), out);
    CHECK(out.size() == 2 * in.size() * 64, "output size %zu", out.size());
    double df = 0;
    auto p = spectrum_db(to_complex(out, 8192, 262144), fs, df);
    CHECK(std::fabs(peak_freq(p, df) - 1500.0) < 2 * df, "baseband tone at %.0f Hz", peak_freq(p, df));
    double worst = -400;
    for (int k = 1; k <= 30; ++k) {
        worst = std::max(worst, level_at(p, df, k * 48000.0 + 1500.0, 200));
        worst = std::max(worst, level_at(p, df, -k * 48000.0 + 1500.0, 200));
    }
    CHECK(worst < -80.0, "CIC images at %.1f dB", worst);
    int peak16 = 0;
    for (size_t i = 8192 * 2; i < out.size(); ++i) peak16 = std::max(peak16, std::abs(static_cast<int>(out[i])));
    CHECK(peak16 > 0.8 * 0.9 * 32767 * 0.97 && peak16 < 0.8 * 0.9 * 32767 * 1.03, "int16 peak %d", peak16);

    // Same tone moved to +700 kHz: nothing left near DC (no LO leakage
    // from the digital side) and no image at -701 kHz.
    nb::Upconverter up2(fs, 0.8);
    up2.set_offset_hz(700000.0);
    out.clear();
    up2.process(in.data(), in.size(), out);
    p = spectrum_db(to_complex(out, 8192, 262144), fs, df);
    CHECK(std::fabs(peak_freq(p, df) - 701500.0) < 2 * df, "shifted tone at %.0f Hz", peak_freq(p, df));
    const double img = level_at(p, df, -701500.0, 300), dc = level_at(p, df, 0.0, 300);
    CHECK(img < -80.0 && dc < -80.0, "image %.1f dB, DC %.1f dB", img, dc);
    // Integer NCO: phase truncation spurs anywhere in the DAC band.
    double spur = -400;
    for (size_t i = 0; i < p.size(); ++i) {
        double f = static_cast<double>(i) * df;
        if (f >= fs / 2) f -= fs;
        if (std::fabs(f - 701500.0) > 3000.0) spur = std::max(spur, p[i]);
    }
    std::printf("  worst spur %.1f dBc\n", spur);
    CHECK(spur < -65.0, "worst spur %.1f dB", spur);
    bool threw = false;
    try {
        up2.set_offset_hz(1.5e6);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw, "offset beyond the DAC band refused");
}

void test_wdsp_sideband() {
    std::printf("WDSP TX: microphone tone comes out as USB, tune carrier\n");
    nb::WdspTx tx(1, 512, 1024);
    std::vector<float> mic(512);
    std::vector<nb::cf> out;
    std::vector<std::complex<double>> all;
    for (int b = 0; b < 200; ++b) {
        for (int i = 0; i < 512; ++i)
            mic[i] = static_cast<float>(0.3 * std::sin(2.0 * kPi * 1000.0 * (b * 512 + i) / 48000.0));
        tx.process(mic.data(), out);
        if (b >= 72)
            for (auto& v : out) all.push_back({v.real(), v.imag()});
    }
    all.resize(65536);
    double df = 0;
    auto p = spectrum_db(all, 48000.0, df);
    const double f = peak_freq(p, df);
    CHECK(std::fabs(f - 1000.0) < 3 * df, "USB tone at %+.0f Hz (want +1000)", f);
    const double opposite = level_at(p, df, -1000.0, 30);
    CHECK(opposite < -40.0, "opposite sideband at %.1f dB", opposite);
    double peak = 0;
    for (auto& v : all) peak = std::max(peak, std::abs(v));
    CHECK(peak > 0.1 && peak <= 1.05, "ALC output peak %.3f", peak);

    tx.set_tune(true, 1500.0, 0.5);
    all.clear();
    std::fill(mic.begin(), mic.end(), 0.0f);
    for (int b = 0; b < 200; ++b) {
        tx.process(mic.data(), out);
        if (b >= 72)
            for (auto& v : out) all.push_back({v.real(), v.imag()});
    }
    all.resize(65536);
    p = spectrum_db(all, 48000.0, df);
    CHECK(std::fabs(peak_freq(p, df) - 1500.0) < 3 * df, "tune carrier at %+.0f Hz (want +1500)", peak_freq(p, df));
}

void test_jitter() {
    std::printf("microphone jitter buffer\n");
    nb::PacketJitter j(3, 6);
    std::string p;
    CHECK(j.pop(p) == nb::PacketJitter::Pop::Empty, "empty at start");
    j.push(10, "a");
    j.push(12, "c");
    CHECK(j.pop(p) == nb::PacketJitter::Pop::Empty, "waits for prefill");
    j.push(11, "b");  // out of order
    j.push(11, "b");  // duplicate
    CHECK(j.size() == 3, "size %zu", j.size());
    CHECK(j.pop(p) == nb::PacketJitter::Pop::Packet && p == "a", "a");
    CHECK(j.pop(p) == nb::PacketJitter::Pop::Packet && p == "b", "b");
    j.push(14, "e");
    CHECK(j.pop(p) == nb::PacketJitter::Pop::Packet && p == "c", "c");
    CHECK(j.pop(p) == nb::PacketJitter::Pop::Missing, "13 missing");
    CHECK(j.pop(p) == nb::PacketJitter::Pop::Packet && p == "e", "e");
    j.push(13, "late");  // after its turn: ignored
    CHECK(j.size() == 0, "late packet dropped");
    CHECK(j.pop(p) == nb::PacketJitter::Pop::Empty && j.underruns() == 1, "underrun counted");
    j.reset();
    for (uint16_t s = 65533; s != 6; ++s) j.push(s, "w");  // wraps, overflows
    CHECK(j.size() == 6 && j.dropped() > 0, "bounded: size %zu dropped %llu", j.size(),
          static_cast<unsigned long long>(j.dropped()));
}

void test_protocol() {
    std::printf("protocol: transmit commands, microphone packets\n");
    std::string err;
    auto c = nb::parse_command("{\"cmd\":\"tx\",\"on\":1}", err);
    CHECK(c.type == nb::Command::Tx && c.a == 1.0, "tx on");
    c = nb::parse_command("{\"cmd\":\"tune\",\"on\":1,\"hz\":1200}", err);
    CHECK(c.type == nb::Command::Tune && c.b == 1200.0, "tune");
    c = nb::parse_command("{\"cmd\":\"tune\",\"on\":0}", err);
    CHECK(c.type == nb::Command::Tune && c.a == 0.0 && c.b == 1000.0, "tune off, default hz");
    err.clear();
    c = nb::parse_command("{\"cmd\":\"txcal\",\"hz\":99999}", err);
    CHECK(c.type == nb::Command::None && !err.empty(), "txcal range");
    c = nb::parse_command("{\"cmd\":\"micgain\",\"db\":6}", err);
    CHECK(c.type == nb::Command::MicGain && c.a == 6.0, "micgain");
    c = nb::parse_command("{\"cmd\":\"comp\",\"on\":1,\"db\":12}", err);
    CHECK(c.type == nb::Command::Comp && c.a == 1.0 && c.b == 12.0, "comp");
    c = nb::parse_command("{\"cmd\":\"comp\",\"on\":1,\"db\":40}", err);
    CHECK(c.type == nb::Command::None && !err.empty(), "comp out of range");
    uint16_t seq = 0;
    std::string opus;
    const std::string m = std::string("\x03\x01\x02", 3) + "OPUS";
    CHECK(nb::parse_mic_packet(m, seq, opus) && seq == 0x0102 && opus == "OPUS", "mic packet");
    CHECK(!nb::parse_mic_packet(std::string("\x01\x00\x01x", 4), seq, opus), "audio type is not a mic packet");
    nb::TxStatus s;
    s.state = "on";
    s.message = "a \"quoted\" reason";
    const std::string j = nb::tx_status_json(s);
    CHECK(j.find("\"state\":\"on\"") != std::string::npos && j.find("\\\"quoted\\\"") != std::string::npos,
          "tx json %s", j.c_str());
}

// Fake DAC: paced like the real one, keeps the IQ and what was done to it.
struct FakeDac : nb::TxSink {
    struct Log {
        std::mutex mu;
        std::vector<int16_t> iq;
        int opens = 0, closes = 0, mutes = 0;
        std::string fail;  // open() throws this when set
        double pace = 1.0; // fraction of rate_ the fake DAC really drains
    };
    Log& log;
    double rate_ = 3072000.0, lo_ = 2399500000.0;
    std::chrono::steady_clock::time_point t0;
    uint64_t written = 0;
    explicit FakeDac(Log& l) : log(l) {}
    void open() override {
        std::lock_guard<std::mutex> lk(log.mu);
        ++log.mutes;  // PlutoTx mutes before opening
        if (!log.fail.empty()) throw std::runtime_error(log.fail);
        ++log.opens;
        t0 = std::chrono::steady_clock::now();
    }
    double rate() const override { return rate_; }
    double lo_hz() const override { return lo_; }
    void write(const int16_t* iq, size_t samples) override {
        {
            std::lock_guard<std::mutex> lk(log.mu);
            if (log.iq.size() < 2 * 4000000) log.iq.insert(log.iq.end(), iq, iq + 2 * samples);
        }
        written += samples;
        double pace;
        {
            std::lock_guard<std::mutex> lk(log.mu);
            pace = log.pace;
        }
        std::this_thread::sleep_until(t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                               std::chrono::duration<double>(written / (rate_ * pace))));
    }
    void close() override {
        std::lock_guard<std::mutex> lk(log.mu);
        ++log.closes;
    }
    void mute() noexcept override {
        std::lock_guard<std::mutex> lk(log.mu);
        ++log.mutes;
    }
};

bool wait_for(const std::function<bool()>& cond, double seconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return cond();
}

void test_transmitter() {
    std::printf("transmitter: Opus microphone -> USB at the uplink offset, safety stops\n");
    FakeDac::Log log;
    nb::TransmitterConfig cfg;
    cfg.max_tx_s = 30;
    nb::Transmitter tx(cfg, [&log]() { return std::make_unique<FakeDac>(log); });
    std::mutex mu;
    std::vector<std::string> states;
    std::atomic<int> emergencies{0};
    tx.on_status = [&](const nb::TxStatus& s) {
        std::lock_guard<std::mutex> lk(mu);
        if (states.empty() || states.back() != s.state) states.push_back(s.state + ":" + s.message);
    };
    tx.on_emergency = [&](const std::string&) { ++emergencies; };

    // Dial 10489.800 MHz -> uplink 2400.300 MHz, 800 kHz above the TX LO.
    tx.set_dial(10489800000.0);
    CHECK(tx.request(1, true).empty(), "request accepted");
    CHECK(!tx.request(2, true).empty(), "second phone refused while the first transmits");

    nb::OpusAudio enc(24000, 5);
    std::vector<float> pcm(960);
    uint16_t seq = 0;
    size_t n = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int k = 0; k < 75; ++k) {  // 1.5 s of microphone, sent in real time
        for (auto& v : pcm) v = static_cast<float>(0.3 * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(n++) / 48000.0));
        enc.push(pcm.data(), pcm.size(), [&](const std::string& p) { tx.mic_packet(1, seq++, p); });
        tx.mic_packet(2, 0, "ignored");  // not the owner
        std::this_thread::sleep_until(start + std::chrono::milliseconds(20 * (k + 1)));
    }
    const auto st = tx.status();
    CHECK(st.state == "on", "state %s (%s)", st.state.c_str(), st.message.c_str());
    CHECK(std::fabs(st.uplink_hz - 2400300000.0) < 1, "uplink %.0f", st.uplink_hz);
    CHECK(st.mic_db > -40 && st.mic_db < 10, "mic meter %.1f dB", st.mic_db);
    tx.request(1, false);
    CHECK(wait_for([&] { return tx.status().state == "off"; }, 2.0), "off after request");
    CHECK(emergencies == 0, "normal stop is not an emergency");
    {
        std::lock_guard<std::mutex> lk(log.mu);
        CHECK(log.opens == 1 && log.closes == 1 && log.mutes >= 2, "open %d close %d mute %d", log.opens, log.closes,
              log.mutes);
        // About 1.5 s at 3.072 MS/s; the tone is 1 kHz above the carrier.
        CHECK(log.iq.size() / 2 > 3072000, "IQ written: %zu samples", log.iq.size() / 2);
        if (log.iq.size() / 2 > 3072000) {
            double df = 0;
            auto p = spectrum_db(to_complex(log.iq, 1536000, 1048576), 3072000.0, df);
            const double f = peak_freq(p, df);
            CHECK(std::fabs(f - 801000.0) < 5 * df, "USB tone at %+.0f Hz from the LO (want +801000)", f);
            const double lsb = level_at(p, df, 799000.0, 20);
            CHECK(lsb < -40.0, "opposite sideband %.1f dB", lsb);
        }
    }

    // The phone goes quiet: nbd stops by itself and releases the Arduino.
    CHECK(tx.request(1, true).empty(), "second transmission");
    CHECK(wait_for([&] { return tx.status().state == "off" && !tx.status().message.empty(); }, 3.0),
          "stops without microphone: %s", tx.status().message.c_str());
    CHECK(emergencies == 1, "emergency after microphone timeout (%d)", emergencies.load());

    // Owner disconnects while transmitting.
    CHECK(tx.request(3, true).empty(), "third transmission");
    CHECK(wait_for([&] { return tx.status().state == "on"; }, 2.0), "on");
    tx.client_closed(3);
    CHECK(wait_for([&] { return tx.status().state == "off"; }, 2.0), "off after disconnect");
    CHECK(emergencies == 2 && tx.status().message == "phone disconnected", "disconnect: %s",
          tx.status().message.c_str());

    // Dial outside the transponder: refused.
    tx.set_dial(10490300000.0);
    CHECK(!tx.request(1, true).empty(), "dial outside the uplink refused");

    // DMA busy (pluto_stream not in pass): error, RF stays muted.
    tx.set_dial(10489800000.0);
    {
        std::lock_guard<std::mutex> lk(log.mu);
        log.fail = "TX DMA busy";
    }
    tx.request(1, true);
    CHECK(wait_for([&] { return tx.status().state == "error"; }, 2.0), "error state: %s", tx.status().state.c_str());
    CHECK(tx.status().message == "TX DMA busy", "message %s", tx.status().message.c_str());

    // The FPGA drains at 1/8 of the rate (x8 interpolator left on): stop.
    {
        std::lock_guard<std::mutex> lk(log.mu);
        log.fail.clear();
        log.pace = 0.125;
    }
    const int before = emergencies;
    CHECK(tx.request(1, true).empty(), "slow DAC transmission");
    CHECK(wait_for([&] { return tx.status().state != "error"; }, 2.0), "slow DAC starts");
    const auto t1 = std::chrono::steady_clock::now();
    for (int k = 0; k < 150 && tx.status().state != "error"; ++k) {
        tx.mic_packet(1, seq++, std::string("\xf8\xff\xfe", 3));
        std::this_thread::sleep_until(t1 + std::chrono::milliseconds(20 * (k + 1)));
    }
    CHECK(tx.status().state == "error" && tx.status().message.find("the DAC takes") == 0, "slow DAC: %s %s",
          tx.status().state.c_str(), tx.status().message.c_str());
    CHECK(emergencies == before + 1, "slow DAC is an emergency");
}

}  // namespace

void test_local_iio_lookup() {
    char tmpl[] = "/tmp/nbtx_sysfsXXXXXX";
    const char* root = mkdtemp(tmpl);
    CHECK(root != nullptr, "mkdtemp");
    if (!root) return;
    const std::string r = root;
    const char* names[] = {"ad9361-phy", "xadc", "cf-ad9361-dds-core-lpc", "cf-ad9361-lpc"};
    for (int i = 0; i < 4; ++i) {
        const std::string d = r + "/iio:device" + std::to_string(i);
        CHECK(std::system(("mkdir -p '" + d + "'").c_str()) == 0, "mkdir");
        std::ofstream(d + "/name") << names[i] << "\n";
    }
    CHECK(nb::find_local_iio_device("cf-ad9361-dds-core-lpc", r) == r + "/iio:device2", "dds core lookup");
    CHECK(nb::find_local_iio_device("cf-ad9361-dds", r).empty(), "no prefix match");
    CHECK(nb::find_local_iio_device("ad9361-phy", "/nonexistent").empty(), "missing root");
    CHECK(std::system(("rm -rf '" + r + "'").c_str()) == 0, "rm");
}

int main() {
    test_local_iio_lookup();
    test_upconverter();
    test_wdsp_sideband();
    test_jitter();
    test_protocol();
    test_transmitter();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all transmit tests passed\n");
    return 0;
}
