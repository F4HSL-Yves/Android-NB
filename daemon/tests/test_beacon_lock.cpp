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
    bool bpsk = false;                 // 400 Bd BPSK instead of CW (centre beacon)
    double dc_amp = 0.0;               // carrier at the stream centre (LO leakage)
    double image_amp = 0.0;            // IQ imbalance: mirror of the beacon (conjugate)
    double image_until = 1e9;          // ...present until then (IQ correction settling)
    double dc_guard = 300.0;           // BeaconLockConfig::dc_guard_hz
    double search = 0.0;               // search half width, 0: config default
    double spur_offset = 0.0;          // fixed carrier (board spur, no LNB drift), Hz from centre
    double spur_amp = 0.0;             // its amplitude (0 = none)
    double ssb_offset = 0.0;           // SSB voice station, suppressed carrier Hz from centre
    double ssb_amp = 0.0;              // its peak amplitude (0 = none)
    double wobble_hz = 0.0;            // LNB error swinging by +/- this (thermostat, sun)...
    double wobble_period = 60.0;       // ...over this many seconds
    double fm_walk = 0.0;              // LNB frequency random walk, Hz per sqrt(s)
    double rate_walk = 0.0;            // LNB drift rate random walk, Hz/s per sqrt(s)
    double loop_hz = 0.0;              // BeaconLockConfig::loop_natural_hz (0: default)
    double burst_every = 0.0;          // a passing carrier near the beacon every this many s (0: none)...
    double burst_len = 0.12, burst_amp = 3.0;  // ...lasting this long, this strong (beacon = 1)
    bool has_hint = false;             // BeaconLockConfig::has_hint / hint_hz
    double hint = 0.0;
};

// A USB voice station: a pitch (90..180 Hz, steady within a syllable,
// gliding a little) and its harmonics in 300..2700 Hz, syllables of
// 0.15..0.4 s separated by short pauses. Squared, a steady pitch puts
// lines between its harmonics, where the plain spectrum has no carrier but
// plenty of signal: the line looks like the BPSK beacon's for a while.
class SsbVoice {
public:
    explicit SsbVoice(double fs) : fs_(fs), rng_(99) { next_syllable(0.0); }
    void sample(double t, double& re, double& im) {
        if (t >= end_) next_syllable(t);
        if (t < start_) return;  // pause
        const double f0 = f0_ + glide_ * (t - start_);
        double env = std::min(1.0, std::min(t - start_, end_ - t) / 0.03);
        for (int h = 1; h < kMaxHarm; ++h) {
            const double f = h * f0;
            if (f < 300.0 || f > 2700.0) continue;
            ph_[h] += 2.0 * kPi * f / fs_;
            if (ph_[h] > kPi) ph_[h] -= 2.0 * kPi;
            const double a = env * amp_[h];
            re += a * std::cos(ph_[h]);
            im += a * std::sin(ph_[h]);
        }
    }

private:
    static constexpr int kMaxHarm = 32;
    void next_syllable(double t) {
        std::uniform_real_distribution<double> u(0.0, 1.0);
        start_ = t + 0.03 + 0.15 * u(rng_);
        end_ = start_ + 0.15 + 0.25 * u(rng_);
        f0_ = 90.0 + 90.0 * u(rng_);
        glide_ = (u(rng_) - 0.5) * 40.0;  // Hz per second
        double norm = 0.0;
        for (int h = 1; h < kMaxHarm; ++h) {
            const double f = h * f0_;
            // Two formant humps, around 600 and 1600 Hz.
            amp_[h] = std::exp(-std::pow((f - 600.0) / 250.0, 2)) + 0.6 * std::exp(-std::pow((f - 1600.0) / 400.0, 2));
            norm += amp_[h];
        }
        for (int h = 1; h < kMaxHarm; ++h) amp_[h] /= std::max(norm, 1e-9);
    }
    double fs_;
    std::mt19937 rng_;
    double start_ = 0.0, end_ = 0.0, f0_ = 120.0, glide_ = 0.0;
    double ph_[kMaxHarm] = {};
    double amp_[kMaxHarm] = {};
};

struct Result {
    int losses = 0;                    // back to searching after the first lock
    double unlocked_s = 0.0;           // time not locked after the first lock
    double rms_error = 0.0;
    double max_error = 0.0;
    double lock_time = -1.0;
    nb::LockState final_state = nb::LockState::Searching;
    bool locked_in_outage = false;
    bool holdover_seen = false;
    bool wrong_lock = false;           // ever locked more than 1 kHz off
    double rate_rms = 0.0, rate_max = 0.0;  // drift estimate error, Hz/s (after 15 s, locked)
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
    cfg.bpsk = sc.bpsk;
    cfg.dc_guard_hz = sc.dc_guard;
    if (sc.search > 0) cfg.search_half_width_hz = sc.search;
    cfg.has_hint = sc.has_hint;
    cfg.hint_hz = sc.hint;
    cfg.loop_natural_hz = sc.loop_hz;
    nb::BeaconLock lock(cfg);
    std::uniform_int_distribution<int> bit(0, 1);
    double symbol = 1.0;
    long last_symbol = -1;

    std::mt19937 rng(1234);
    // Beacon amplitude 1: noise power per complex sample = fs / 10^(CN0/10).
    const double noise_power = sc.fs / std::pow(10.0, sc.cn0_dbhz / 10.0);
    std::normal_distribution<float> gauss(0.0f, static_cast<float>(std::sqrt(noise_power / 2.0)));

    const size_t block = 4096;
    std::vector<nb::cf> buf(block);
    double ph_b = 0.0, ph_i = 0.0, ph_s = 0.0, ph_v = 0.0;
    SsbVoice voice(sc.fs);
    std::mt19937 walk_rng(777);
    std::mt19937 burst_rng(4242);
    std::uniform_real_distribution<double> burst_off(-45.0, 45.0);
    double ph_x = 0.0, burst_hz = 0.0;
    long burst_k = -1;
    std::normal_distribution<double> walk_gauss(0.0, 1.0);
    const double walk_step = sc.fm_walk / std::sqrt(sc.fs);
    double walk = 0.0, truth = sc.lnb_error0, rwalk = 0.0, rint = 0.0, prev_truth = sc.lnb_error0, rate_sq = 0.0;
    size_t n_rate = 0;
    const double rwalk_step = sc.rate_walk / std::sqrt(sc.fs);
    nb::LockState prev_state = nb::LockState::Searching;
    double sum_sq = 0.0, max_err = 0.0;
    size_t n_err = 0;
    Result r;
    const size_t total = static_cast<size_t>(sc.seconds * sc.fs);
    for (size_t pos = 0; pos < total; pos += block) {
        for (size_t i = 0; i < block; ++i) {
            const double t = (pos + i) / sc.fs;
            walk += walk_step * walk_gauss(walk_rng);
            if (sc.rate_walk > 0.0) {
                rwalk += rwalk_step * walk_gauss(walk_rng);
                rint += rwalk / sc.fs;
            }
            const double err = sc.lnb_error0 + sc.drift * t + walk + rint +
                               sc.wobble_hz * std::sin(2.0 * kPi * t / sc.wobble_period);
            truth = err;
            const double fb = sc.beacon_offset + err;
            ph_b += 2.0 * kPi * fb / sc.fs;
            ph_i += 2.0 * kPi * (fb + sc.interferer_offset) / sc.fs;
            if (ph_b > kPi) ph_b -= 2.0 * kPi;
            if (ph_i > kPi) ph_i -= 2.0 * kPi;
            const bool outage = t >= sc.outage_from && t < sc.outage_to;
            double a = (!outage && (!sc.keyed || key_down(t))) ? 1.0 : 0.0;
            if (sc.bpsk) {
                const long k = static_cast<long>(t * 400.0);
                if (k != last_symbol) {
                    last_symbol = k;
                    symbol = bit(rng) ? 1.0 : -1.0;
                }
                a *= symbol;
            }
            double re = a * std::cos(ph_b) + sc.dc_amp, im = a * std::sin(ph_b);
            if (sc.image_amp > 0.0 && t < sc.image_until) {
                re += sc.image_amp * a * std::cos(ph_b);
                im -= sc.image_amp * a * std::sin(ph_b);
            }
            if (sc.spur_amp > 0.0) {
                ph_s += 2.0 * kPi * sc.spur_offset / sc.fs;
                if (ph_s > kPi) ph_s -= 2.0 * kPi;
                re += sc.spur_amp * std::cos(ph_s);
                im += sc.spur_amp * std::sin(ph_s);
            }
            if (sc.ssb_amp > 0.0) {
                double vr = 0.0, vi = 0.0;
                voice.sample(t, vr, vi);
                ph_v += 2.0 * kPi * (sc.ssb_offset + 0.3 * t) / sc.fs;  // the station drifts a little
                if (ph_v > kPi) ph_v -= 2.0 * kPi;
                const double c = std::cos(ph_v), sn = std::sin(ph_v);
                re += sc.ssb_amp * (vr * c - vi * sn);
                im += sc.ssb_amp * (vr * sn + vi * c);
            }
            if (sc.burst_every > 0.0) {
                const long k = static_cast<long>(t / sc.burst_every);
                if (k != burst_k) {
                    burst_k = k;
                    burst_hz = burst_off(burst_rng);
                }
                if (k > 0 && t - k * sc.burst_every < sc.burst_len) {
                    // Squared (BPSK), a carrier at f shows at 2 f: put it half
                    // way so the tracking sees burst_hz off the beacon.
                    ph_x += 2.0 * kPi * (fb + (sc.bpsk ? 0.5 : 1.0) * burst_hz) / sc.fs;
                    if (ph_x > kPi) ph_x -= 2.0 * kPi;
                    re += sc.burst_amp * std::cos(ph_x);
                    im += sc.burst_amp * std::sin(ph_x);
                }
            }
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
        if (r.lock_time > 0 && st.state == nb::LockState::Searching && prev_state != nb::LockState::Searching) ++r.losses;
        if (r.lock_time > 0 && st.state != nb::LockState::Locked) r.unlocked_s += block / sc.fs;
        prev_state = st.state;
        if (st.state != nb::LockState::Searching &&
            std::abs(st.correction_hz - truth) > 1000.0)
            r.wrong_lock = true;
        const bool outage = t >= sc.outage_from && t < sc.outage_to;
        if (outage && t > sc.outage_from + 6.0 && st.state == nb::LockState::Locked) r.locked_in_outage = true;
        const double true_rate = (truth - prev_truth) / (block / sc.fs);
        prev_truth = truth;
        if (t > 15.0 && st.state == nb::LockState::Locked) {
            const double re = st.drift_hz_per_s - true_rate;
            rate_sq += re * re;
            r.rate_max = std::max(r.rate_max, std::abs(re));
            ++n_rate;
        }
        if (t > 15.0 && st.state != nb::LockState::Searching) {
            const double e = st.correction_hz - truth;
            sum_sq += e * e;
            max_err = std::max(max_err, std::abs(e));
            ++n_err;
        }
    }
    r.rms_error = n_err ? std::sqrt(sum_sq / n_err) : 1e9;
    r.rate_rms = n_rate ? std::sqrt(rate_sq / n_rate) : 1e9;
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
    {
        Scenario sc;
        sc.bpsk = true;
        sc.keyed = false;
        const Result r = run(sc);
        report("BPSK beacon (squared)", r);
        CHECK(r.lock_time > 0 && r.lock_time < 3.0, "lock time %.2f", r.lock_time);
        CHECK(r.rms_error < 1.5, "rms %.3f", r.rms_error);
        CHECK(r.final_state == nb::LockState::Locked, "final state");
    }
    {
        // The centre beacon sits at the stream centre: LO leakage must not win.
        Scenario sc;
        sc.bpsk = true;
        sc.keyed = false;
        sc.beacon_offset = 0.0;
        sc.lnb_error0 = 21000.0;
        sc.drift = 0.5;
        sc.dc_amp = 2.0;
        sc.interferer_offset = 0.0;
        const Result r = run(sc);
        report("BPSK beacon at centre, DC spur", r);
        CHECK(r.lock_time > 0 && r.lock_time < 3.0, "lock time %.2f", r.lock_time);
        CHECK(r.rms_error < 1.5, "rms %.3f", r.rms_error);
    }

    {
        // GPS-locked LNB: the centre beacon sits within a few tens of Hz of
        // the stream centre. On the board (no DC there, guard off) it locks.
        Scenario sc;
        sc.bpsk = true;
        sc.keyed = false;
        sc.beacon_offset = 0.0;
        sc.lnb_error0 = 40.0;
        sc.drift = 0.05;
        sc.interferer_offset = 0.0;
        sc.dc_guard = 0.0;
        const Result r = run(sc);
        report("BPSK beacon, precise LNB", r);
        CHECK(!r.wrong_lock, "wrong lock");
        CHECK(r.lock_time > 0 && r.lock_time < 3.0, "lock time %.2f", r.lock_time);
        CHECK(r.rms_error < 1.5, "rms %.3f", r.rms_error);
        CHECK(r.final_state == nb::LockState::Locked, "final state");
    }
    {
        // LNB +34 kHz off (seen on a board): the BPSK search (60 kHz in
        // nbd, swept in three 25 kHz windows) still finds the centre beacon,
        // despite its image at -34 kHz and their product at the centre.
        Scenario sc;
        sc.bpsk = true;
        sc.keyed = false;
        sc.beacon_offset = 0.0;
        sc.lnb_error0 = 34000.0;
        sc.drift = 0.5;
        sc.interferer_offset = 0.0;
        sc.image_amp = 0.1;
        sc.dc_guard = 0.0;
        sc.search = 60000.0;
        const Result r = run(sc);
        report("BPSK beacon, LNB +34 kHz", r);
        CHECK(!r.wrong_lock, "wrong lock");
        CHECK(r.lock_time > 0 && r.lock_time < 4.5, "lock time %.2f", r.lock_time);
        CHECK(r.rms_error < 1.5, "rms %.3f", r.rms_error);
    }
    {
        // Same LNB with the CW beacon (25 kHz search): the beacon is out of
        // the window and a carrier sits against its edge. Searching is
        // right; locking on the carrier 24 kHz off is not.
        Scenario sc;
        sc.lnb_error0 = 34000.0;
        sc.drift = 0.0;
        sc.interferer_offset = 24400.0 - 34000.0;
        sc.interferer_amp = 1.0;
        sc.seconds = 20.0;
        const Result r = run(sc);
        report("CW beacon out of reach, edge carrier", r);
        CHECK(!r.wrong_lock, "locked on the edge carrier");
    }
    {
        // Seen on the FishBall (LNB +24.7 kHz): a steady carrier 16 kHz
        // below the stream centre, without the LNB drift (a spur of the
        // board itself), squares into a cleaner line than the BPSK beacon
        // and won the sweep. The beacon has no carrier: a line that is also
        // a carrier in the plain spectrum is not the beacon.
        Scenario sc;
        sc.bpsk = true;
        sc.keyed = false;
        sc.beacon_offset = 0.0;
        sc.lnb_error0 = 24700.0;
        sc.drift = 0.3;
        sc.interferer_offset = 0.0;
        sc.dc_guard = 0.0;
        sc.search = 60000.0;
        sc.spur_offset = -16000.0;
        sc.spur_amp = 3.0;
        const Result r = run(sc);
        report("BPSK, LNB +24.7 kHz, spur -16 kHz", r);
        CHECK(!r.wrong_lock, "locked on the spur");
        CHECK(r.lock_time > 0 && r.lock_time < 6.0, "lock time %.2f", r.lock_time);
        CHECK(r.rms_error < 1.5, "rms %.3f", r.rms_error);
    }
    {
        // Same with a weak beacon (42 dB-Hz): the checks on the plain
        // spectrum must not throw the real beacon away.
        Scenario sc;
        sc.bpsk = true;
        sc.keyed = false;
        sc.beacon_offset = 0.0;
        sc.lnb_error0 = 24700.0;
        sc.drift = 0.3;
        sc.interferer_offset = 0.0;
        sc.dc_guard = 0.0;
        sc.search = 60000.0;
        sc.cn0_dbhz = 42.0;
        sc.spur_offset = -16000.0;
        sc.spur_amp = 0.5;
        const Result r = run(sc);
        report("weak BPSK, LNB +24.7 kHz, spur", r);
        CHECK(!r.wrong_lock, "locked on the spur");
        CHECK(r.lock_time > 0 && r.lock_time < 10.0, "lock time %.2f", r.lock_time);
        CHECK(r.rms_error < 3.0, "rms %.3f", r.rms_error);
    }
    {
        // Seen on the FishBall with v1.19 (LNB +24.7 kHz): a strong SSB
        // station 21 kHz below the stream centre won the sweep, and the loop
        // then followed its voice (drift up to 27 Hz/s). Whatever the
        // acquisition picks must stay a steady line before it counts as
        // the beacon.
        Scenario sc;
        sc.bpsk = true;
        sc.keyed = false;
        sc.beacon_offset = 0.0;
        sc.lnb_error0 = 24700.0;
        sc.drift = 0.3;
        sc.interferer_offset = 0.0;
        sc.dc_guard = 0.0;
        sc.search = 60000.0;
        sc.ssb_offset = -20900.0;
        sc.ssb_amp = 8.0;
        sc.seconds = 60.0;
        const Result r = run(sc);
        report("BPSK, LNB +24.7 kHz, SSB -21 kHz", r);
        CHECK(!r.wrong_lock, "locked on the SSB station");
        CHECK(r.lock_time > 0 && r.lock_time < 15.0, "lock time %.2f", r.lock_time);
        CHECK(r.rms_error < 1.5, "rms %.3f", r.rms_error);
        CHECK(r.final_state == nb::LockState::Locked, "final state");
    }
    {
        // Louder still.
        // Seen on the FishBall with v1.19 (LNB +24.7 kHz): a strong SSB
        // station 21 kHz below the stream centre won the sweep, and the loop
        // then followed its voice (drift up to 27 Hz/s). Whatever the
        // acquisition picks must stay a steady line before it counts as
        // the beacon.
        Scenario sc;
        sc.bpsk = true;
        sc.keyed = false;
        sc.beacon_offset = 0.0;
        sc.lnb_error0 = 24700.0;
        sc.drift = 0.3;
        sc.interferer_offset = 0.0;
        sc.dc_guard = 0.0;
        sc.search = 60000.0;
        sc.ssb_offset = -20900.0;
        sc.ssb_amp = 25.0;
        sc.seconds = 60.0;
        const Result r = run(sc);
        report("BPSK, LNB +24.7 kHz, SSB loud", r);
        CHECK(!r.wrong_lock, "locked on the SSB station");
        CHECK(r.lock_time > 0 && r.lock_time < 15.0, "lock time %.2f", r.lock_time);
        CHECK(r.rms_error < 1.5, "rms %.3f", r.rms_error);
        CHECK(r.final_state == nb::LockState::Locked, "final state");
    }
    {
        // A friend's LNB further off still: the BPSK search (100 kHz in
        // nbd, five 25 kHz windows) reaches it, either side.
        for (const double off : {-92000.0, 88000.0, 100000.0, -102000.0}) {
            Scenario sc;
            sc.bpsk = true;
            sc.keyed = false;
            sc.beacon_offset = 0.0;
            sc.lnb_error0 = off;
            sc.drift = 0.5;
            sc.interferer_offset = 0.0;
            sc.image_amp = 0.1;
            sc.dc_guard = 0.0;
            sc.search = 100000.0;
            sc.ssb_offset = 30000.0;
            sc.ssb_amp = 8.0;
            const Result r = run(sc);
            char name[48];
            std::snprintf(name, sizeof name, "BPSK, LNB %+.0f kHz, SSB", off / 1000.0);
            report(name, r);
            CHECK(!r.wrong_lock, "wrong lock");
            CHECK(r.lock_time > 0 && r.lock_time < 15.0, "lock time %.2f", r.lock_time);
            CHECK(r.rms_error < 1.5, "rms %.3f", r.rms_error);
        }
    }
    {
        // Seen at a friend's (v1.20, v1.21): the PSK beacon 74 kHz below,
        // clearly visible, never locked. The check against voice stations
        // wanted a clean line in nearly every 40 ms block, which a weaker
        // beacon, or one that had drifted since its window was swept,
        // fails. Weak, drifting or both, it must lock.
        const struct { double cn0, drift; const char* name; } cases[] = {
            {38.0, 0.3, "weak BPSK (38 dB-Hz), LNB -74 kHz"},
            {50.0, 15.0, "BPSK, LNB -74 kHz drifting 15 Hz/s"},
            {40.0, 8.0, "weak BPSK, LNB -74 kHz drifting"},
        };
        for (const auto& c : cases) {
            Scenario sc;
            sc.bpsk = true;
            sc.keyed = false;
            sc.beacon_offset = 0.0;
            sc.lnb_error0 = -74000.0;
            sc.drift = c.drift;
            sc.interferer_offset = 0.0;
            sc.dc_guard = 0.0;
            sc.search = 100000.0;
            sc.cn0_dbhz = c.cn0;
            const Result r = run(sc);
            report(c.name, r);
            CHECK(!r.wrong_lock, "wrong lock");
            CHECK(r.lock_time > 0 && r.lock_time < 15.0, "lock time %.2f", r.lock_time);
            CHECK(r.rms_error < 3.0, "rms %.3f", r.rms_error);
            CHECK(r.final_state == nb::LockState::Locked, "final state");
        }
    }
    {
        // At the same friend's, after v1.22: the beacon still got lost now
        // and then. A LNB without PLL does not drift steadily: its error
        // swings (thermostat, sun, wind) and wanders. The loop must follow
        // it without letting go.
        const struct { double cn0, wobble, walk; const char* name; } cases[] = {
            {50.0, 1000.0, 0.0, "BPSK, LNB swinging +/-1 kHz / 60 s"},
            {50.0, 0.0, 20.0, "BPSK, LNB wandering 20 Hz/sqrt(s)"},
            {40.0, 1000.0, 0.0, "weak BPSK, LNB swinging"},
            {40.0, 0.0, 20.0, "weak BPSK, LNB wandering"},
        };
        for (const auto& c : cases) {
            Scenario sc;
            sc.bpsk = true;
            sc.keyed = false;
            sc.beacon_offset = 0.0;
            sc.lnb_error0 = -74000.0;
            sc.drift = 0.0;
            sc.interferer_offset = 0.0;
            sc.dc_guard = 0.0;
            sc.search = 100000.0;
            sc.cn0_dbhz = c.cn0;
            sc.wobble_hz = c.wobble;
            sc.fm_walk = c.walk;
            sc.seconds = 180.0;
            const Result r = run(sc);
            report(c.name, r);
            CHECK(!r.wrong_lock, "wrong lock");
            CHECK(r.lock_time > 0 && r.lock_time < 25.0, "lock time %.2f", r.lock_time);
            CHECK(r.losses == 0, "%d losses, %.0f s unlocked", r.losses, r.unlocked_s);
            // v1.29: a calmer loop (0.3 Hz) follows a wandering LNB a little less
            // closely, in exchange for not running away on a bad block.
            CHECK(r.rms_error < 12.0, "rms %.3f", r.rms_error);
        }
    }
    {
        // Seen on a FishBall (v1.28, beacon at SNR 12 dB, LNB nearly still):
        // the drift estimate shook between -19 and +16 Hz/s, then one bad
        // block threw it to +28 Hz/s; extrapolated, it lost the beacon, and
        // the next search locked 105 kHz off. Weak beacon, LNB still, with
        // or without a passing carrier every 5 s: no loss, a calm estimate.
        const struct { double cn0, burst; const char* name; } cases[] = {
            {38.0, 0.0, "weak BPSK, LNB still"},
            {38.0, 5.0, "weak BPSK, passing carriers"},
            {45.0, 5.0, "BPSK, passing carriers"},
        };
        for (const auto& c : cases) {
            Scenario sc;
            sc.bpsk = true;
            sc.keyed = false;
            sc.beacon_offset = 0.0;
            sc.lnb_error0 = 21000.0;
            sc.drift = 0.0;
            sc.interferer_offset = 0.0;
            sc.dc_guard = 0.0;
            sc.search = 100000.0;
            sc.has_hint = true;
            sc.hint = 21000.0;
            sc.cn0_dbhz = c.cn0;
            sc.burst_every = c.burst;
            sc.seconds = 200.0;
            const Result r = run(sc);
            report(c.name, r);
            std::printf("%-32s drift estimate rms %.2f Hz/s, max %.2f Hz/s\n", "", r.rate_rms, r.rate_max);
            CHECK(!r.wrong_lock, "wrong lock");
            CHECK(r.losses == 0, "%d losses", r.losses);
            CHECK(r.rate_rms < 3.0, "drift rms %.2f", r.rate_rms);
            CHECK(r.rate_max < 12.0, "drift max %.2f", r.rate_max);
            CHECK(r.max_error < 12.0, "max %.3f", r.max_error);
        }
    }
    {
        // Just after start the AD9363's IQ correction has not settled: the
        // beacon's mirror is briefly stronger than the beacon itself (seen
        // on the board: a lock at -21.9 kHz instead of +21.7 kHz).
        Scenario sc;
        sc.bpsk = true;
        sc.keyed = false;
        sc.beacon_offset = 80.0;
        sc.lnb_error0 = 21700.0;
        sc.drift = 0.5;
        sc.interferer_offset = 0.0;
        sc.image_amp = 1.5;
        sc.image_until = 1.5;
        const Result r = run(sc);
        report("BPSK, strong image at start", r);
        CHECK(!r.wrong_lock, "locked on the image");
        CHECK(r.lock_time > 0 && r.lock_time < 6.0, "lock time %.2f", r.lock_time);
        CHECK(r.rms_error < 1.5, "rms %.3f", r.rms_error);
    }
    {
        // A residual image after IQ correction (-20 dB) is harmless.
        Scenario sc;
        sc.bpsk = true;
        sc.keyed = false;
        sc.beacon_offset = 80.0;
        sc.lnb_error0 = 21700.0;
        sc.drift = 0.5;
        sc.interferer_offset = 0.0;
        sc.image_amp = 0.1;
        const Result r = run(sc);
        report("BPSK, residual image", r);
        CHECK(!r.wrong_lock, "locked on the image");
        CHECK(r.lock_time > 0 && r.lock_time < 6.0, "lock time %.2f", r.lock_time);
        CHECK(r.rms_error < 1.5, "rms %.3f", r.rms_error);
    }
    {
        // CW beacon with a strong image at start, mirror in the window.
        Scenario sc;
        sc.beacon_offset = -2000.0;
        sc.lnb_error0 = 6000.0;
        sc.interferer_offset = 0.0;
        sc.image_amp = 1.5;
        sc.image_until = 1.5;
        const Result r = run(sc);
        report("CW, strong image at start", r);
        CHECK(!r.wrong_lock, "locked on the image");
        CHECK(r.rms_error < 1.0, "rms %.3f", r.rms_error);
    }

    {
        // nbd's BPSK search (+/-100 kHz) sweeps five windows before it locks.
        Scenario sc;
        sc.bpsk = true;
        sc.keyed = false;
        sc.search = 100000.0;
        sc.lnb_error0 = 24700.0;
        sc.interferer_offset = 0.0;
        const Result r = run(sc);
        report("BPSK +/-100 kHz, no hint", r);
        CHECK(!r.wrong_lock, "wrong lock");
        CHECK(r.lock_time > 0 && r.lock_time < 10.0, "lock time %.2f", r.lock_time);
        // The LNB moved by 1.8 kHz since the last session: the hint finds it at once.
        sc.has_hint = true;
        sc.hint = 22900.0;
        const Result h = run(sc);
        report("BPSK +/-100 kHz, hint 1.8 kHz off", h);
        CHECK(!h.wrong_lock, "wrong lock");
        CHECK(h.lock_time > 0 && h.lock_time < 3.0, "lock time %.2f", h.lock_time);
        CHECK(h.lock_time < r.lock_time, "hint not faster (%.2f vs %.2f)", h.lock_time, r.lock_time);
        CHECK(h.rms_error < 1.5, "rms %.3f", h.rms_error);
        CHECK(h.final_state == nb::LockState::Locked, "final state");
    }
    {
        // A stale hint (other LNB): one window for nothing, then the sweep.
        Scenario sc;
        sc.bpsk = true;
        sc.keyed = false;
        sc.search = 100000.0;
        sc.lnb_error0 = -31000.0;
        sc.interferer_offset = 0.0;
        sc.has_hint = true;
        sc.hint = 40000.0;
        const Result r = run(sc);
        report("BPSK, stale hint 71 kHz off", r);
        CHECK(!r.wrong_lock, "wrong lock");
        CHECK(r.lock_time > 0 && r.lock_time < 12.0, "lock time %.2f", r.lock_time);
        CHECK(r.rms_error < 1.5, "rms %.3f", r.rms_error);
        CHECK(r.final_state == nb::LockState::Locked, "final state");
    }
    {
        // Hint 3 kHz off, an SSB voice near it: the BPSK check rejects the
        // voice and the beacon is still found.
        Scenario sc;
        sc.bpsk = true;
        sc.keyed = false;
        sc.search = 100000.0;
        sc.lnb_error0 = 12000.0;
        sc.interferer_offset = 0.0;
        sc.ssb_offset = -250000.0 + 15000.0 + 1500.0;
        sc.ssb_amp = 3.0;
        sc.has_hint = true;
        sc.hint = 15000.0;
        const Result r = run(sc);
        report("BPSK, hint near a voice", r);
        CHECK(!r.wrong_lock, "wrong lock");
        CHECK(r.lock_time > 0 && r.lock_time < 12.0, "lock time %.2f", r.lock_time);
        CHECK(r.final_state == nb::LockState::Locked, "final state");
    }
    {
        // CW with a hint: same lock as without, and the hint shows before it.
        Scenario sc;
        sc.lnb_error0 = -9000.0;
        sc.has_hint = true;
        sc.hint = -8000.0;
        const Result r = run(sc);
        report("CW, hint 1 kHz off", r);
        CHECK(!r.wrong_lock, "wrong lock");
        CHECK(r.lock_time > 0 && r.lock_time < 3.0, "lock time %.2f", r.lock_time);
        CHECK(r.rms_error < 1.0, "rms %.3f", r.rms_error);
        nb::BeaconLockConfig cfg;
        cfg.has_hint = true;
        cfg.hint_hz = -8000.0;
        CHECK(std::abs(nb::BeaconLock(cfg).correction_hz() + 8000.0) < 1e-6, "hint not reported before the lock");
    }

    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("all beacon lock tests passed\n");
    return EXIT_SUCCESS;
}
