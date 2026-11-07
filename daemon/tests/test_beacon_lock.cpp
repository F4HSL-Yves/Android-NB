// Beacon lock tests on synthetic IQ: a keyed CW beacon whose frequency
// drifts like a warming LNB, white noise, and an interfering carrier.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "beacon_lock.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
int failures = 0;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                        \
            std::printf("\n");                               \
            ++failures;                                      \
        }                                                    \
    } while (0)

struct Scenario {
    double fs = 768000.0;
    double beacon_offset = -250000.0;  // expected beacon position
    double lnb_error0 = 2500.0;        // initial LNB error, Hz
    double drift = 1.5;                // Hz per second
    double cn0_dbhz = 50.0;            // beacon carrier to noise density
    double interferer_offset = 3000.0; // relative to the beacon, Hz (0 = none)
    double interferer_amp = 0.5;   // weaker than the beacon, as the band plan requires
    double seconds = 40.0;
    bool keyed = true;                 // CW keying on/off
    double outage_from = -1.0, outage_to = -1.0;  // beacon absent in [from, to)
};

struct Result {
    double rms_error = 0.0;
    double max_error = 0.0;
    double lock_time = -1.0;
    nb::LockState final_state = nb::LockState::Searching;
    bool locked_in_outage = false;
    bool holdover_seen = false;
};

// Keying pattern: dots, dashes and gaps with roughly 60 % duty cycle.
bool key_down(double t) {
    static const double pattern[] = {0.24, 0.08, 0.08, 0.08, 0.24, 0.24, 0.08, 0.08, 0.24, 0.16};
    const double period = 1.52;
    double x = std::fmod(t, period);
    bool on = true;
    for (double d : pattern) {
        if (x < d) return on;
        x -= d;
        on = !on;
    }
    return false;
}

Result run(const Scenario& sc) {
    nb::BeaconLockConfig cfg;
    cfg.sample_rate = sc.fs;
    cfg.beacon_offset_hz = sc.beacon_offset;
    nb::BeaconLock lock(cfg);

    std::mt19937 rng(1234);
    // Beacon amplitude 1: noise power per complex sample = fs / 10^(CN0/10).
    const double noise_power = sc.fs / std::pow(10.0, sc.cn0_dbhz / 10.0);
    std::normal_distribution<float> gauss(0.0f, static_cast<float>(std::sqrt(noise_power / 2.0)));

    const size_t block = 4096;
    std::vector<nb::cf> buf(block);
    double ph_b = 0.0, ph_i = 0.0;
    double sum_sq = 0.0, max_err = 0.0;
    size_t n_err = 0;
    Result r;
    const size_t total = static_cast<size_t>(sc.seconds * sc.fs);
    for (size_t pos = 0; pos < total; pos += block) {
        for (size_t i = 0; i < block; ++i) {
            const double t = (pos + i) / sc.fs;
            const double err = sc.lnb_error0 + sc.drift * t;
            const double fb = sc.beacon_offset + err;
            ph_b += 2.0 * kPi * fb / sc.fs;
            ph_i += 2.0 * kPi * (fb + sc.interferer_offset) / sc.fs;
            if (ph_b > kPi) ph_b -= 2.0 * kPi;
            if (ph_i > kPi) ph_i -= 2.0 * kPi;
            const bool outage = t >= sc.outage_from && t < sc.outage_to;
            const double a = (!outage && (!sc.keyed || key_down(t))) ? 1.0 : 0.0;
            double re = a * std::cos(ph_b), im = a * std::sin(ph_b);
            if (sc.interferer_offset != 0.0) {
                re += sc.interferer_amp * std::cos(ph_i);
                im += sc.interferer_amp * std::sin(ph_i);
            }
            buf[i] = nb::cf(static_cast<float>(re) + gauss(rng), static_cast<float>(im) + gauss(rng));
        }
        lock.process(buf.data(), block);

        const double t = (pos + block) / sc.fs;
        const auto st = lock.status();
        if (st.state == nb::LockState::Locked && r.lock_time < 0) r.lock_time = t;
        if (st.state == nb::LockState::Holdover) r.holdover_seen = true;
        const bool outage = t >= sc.outage_from && t < sc.outage_to;
        if (outage && t > sc.outage_from + 6.0 && st.state == nb::LockState::Locked) r.locked_in_outage = true;
        if (t > 15.0 && st.state != nb::LockState::Searching) {
            const double e = st.correction_hz - (sc.lnb_error0 + sc.drift * t);
            sum_sq += e * e;
            max_err = std::max(max_err, std::abs(e));
            ++n_err;
        }
    }
    r.rms_error = n_err ? std::sqrt(sum_sq / n_err) : 1e9;
    r.max_error = n_err ? max_err : 1e9;
    r.final_state = lock.state();
    return r;
}

void report(const char* name, const Result& r) {
    std::printf("%-32s lock at %5.2f s, rms %.3f Hz, max %.3f Hz, final %s\n", name, r.lock_time,
                r.rms_error, r.max_error, nb::to_string(r.final_state));
}

}  // namespace

int main() {
    {
        Scenario sc;
        const Result r = run(sc);
        report("strong beacon, drifting LNB", r);
        CHECK(r.lock_time > 0 && r.lock_time < 3.0, "lock time %.2f", r.lock_time);
        CHECK(r.rms_error < 1.0, "rms %.3f", r.rms_error);
        CHECK(r.max_error < 3.0, "max %.3f", r.max_error);
        CHECK(r.final_state == nb::LockState::Locked, "final state");
    }
    {
        Scenario sc;
        sc.cn0_dbhz = 40.0;
        const Result r = run(sc);
        report("weak beacon (40 dB-Hz)", r);
        CHECK(r.lock_time > 0 && r.lock_time < 5.0, "lock time %.2f", r.lock_time);
        CHECK(r.rms_error < 3.0, "rms %.3f", r.rms_error);
    }
    {
        Scenario sc;
        sc.lnb_error0 = -18000.0;
        sc.drift = -3.0;
        sc.interferer_offset = 0.0;
        const Result r = run(sc);
        report("LNB 18 kHz low, fast drift", r);
        CHECK(r.lock_time > 0 && r.lock_time < 3.0, "lock time %.2f", r.lock_time);
        CHECK(r.rms_error < 1.0, "rms %.3f", r.rms_error);
    }
    {
        Scenario sc;
        sc.keyed = false;
        const Result r = run(sc);
        report("unkeyed carrier", r);
        CHECK(r.rms_error < 1.0, "rms %.3f", r.rms_error);
    }
    {
        Scenario sc;
        sc.seconds = 50.0;
        sc.outage_from = 20.0;
        sc.outage_to = 32.0;
        const Result r = run(sc);
        report("12 s beacon outage", r);
        CHECK(r.holdover_seen, "holdover never reported");
        CHECK(!r.locked_in_outage, "claimed lock during outage");
        CHECK(r.final_state == nb::LockState::Locked, "did not relock after outage");
        CHECK(r.max_error < 5.0, "max %.3f", r.max_error);
    }

    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("all beacon lock tests passed\n");
    return EXIT_SUCCESS;
}
