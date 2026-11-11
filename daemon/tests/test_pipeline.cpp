// End to end: beacon lock + WDSP USB demodulation on synthetic IQ.
// A station transmits a 1000 Hz tone in USB; the LNB is 2.5 kHz off and
// drifting. With the lock, the audio tone must come out at 1000 Hz.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "beacon_lock.h"
#include "dsp.h"
#include "wdsp_rx.h"

namespace {
constexpr double kPi = 3.14159265358979323846;

// Strongest audio frequency over the last part of the recording.
double dominant_hz(const std::vector<float>& audio, size_t len) {
    std::vector<std::complex<double>> s(len);
    const size_t start = audio.size() - len;
    for (size_t i = 0; i < len; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (len - 1));
        s[i] = audio[start + i] * w;
    }
    nb::fft_inplace(s);
    size_t best = 1;
    for (size_t k = 1; k < len / 2; ++k)
        if (std::norm(s[k]) > std::norm(s[best])) best = k;
    const double a = std::log(std::norm(s[best - 1]) + 1e-30), b = std::log(std::norm(s[best]) + 1e-30),
                 c = std::log(std::norm(s[best + 1]) + 1e-30);
    const double d = 0.5 * (a - c) / (a - 2.0 * b + c);
    return (best + d) * nb::WdspRx::kAudioRate / len;
}
}  // namespace

int main() {
    const double fs = 768000.0, center = 10489750000.0, beacon = 10489500000.0, dial = 10489900000.0;
    const double err0 = 2500.0, drift = 1.0;

    nb::BeaconLockConfig cfg;
    cfg.sample_rate = fs;
    cfg.beacon_offset_hz = beacon - center;
    nb::BeaconLock lock(cfg);
    const int block = 4096;
    nb::WdspRx rx(0, static_cast<int>(fs), block);

    std::mt19937 rng(7);
    std::normal_distribution<float> gauss(0.0f, 0.05f);
    std::vector<nb::cf> iq(block);
    std::vector<float> audio;
    double ph_b = 0.0, ph_s = 0.0;
    const size_t total = static_cast<size_t>(20.0 * fs);
    for (size_t pos = 0; pos + block <= total; pos += block) {
        for (int i = 0; i < block; ++i) {
            const double t = (pos + i) / fs;
            const double e = err0 + drift * t;
            ph_b = std::fmod(ph_b + 2.0 * kPi * (beacon - center + e) / fs, 2.0 * kPi);
            ph_s = std::fmod(ph_s + 2.0 * kPi * (dial + 1000.0 - center + e) / fs, 2.0 * kPi);
            iq[i] = nb::cf(static_cast<float>(std::cos(ph_b) + 0.3 * std::cos(ph_s)) + gauss(rng),
                           static_cast<float>(std::sin(ph_b) + 0.3 * std::sin(ph_s)) + gauss(rng));
        }
        lock.process(iq.data(), block);
        rx.set_offset_hz(dial - center + lock.correction_hz());
        rx.process(iq.data(), audio);
    }
    const double tone = dominant_hz(audio, 65536);
    std::printf("beacon %s, correction %.1f Hz (true %.1f), audio tone %.2f Hz\n",
                nb::to_string(lock.state()), lock.correction_hz(), err0 + drift * 20.0, tone);
    if (lock.state() != nb::LockState::Locked || std::abs(tone - 1000.0) > 2.0) {
        std::printf("FAIL: expected a 1000 Hz tone with the beacon locked\n");
        return EXIT_FAILURE;
    }
    std::printf("pipeline test passed\n");
    return EXIT_SUCCESS;
}
