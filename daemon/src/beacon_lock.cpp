#include "beacon_lock.h"

#include <algorithm>
#include <cmath>

namespace nb {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kZeta = 0.707;
// |mean phasor product| / mean magnitude above which a 40 ms block counts
// as a clean carrier. Noise alone gives about 1/sqrt(40) = 0.16, and
// exceeds the gate with odds of roughly exp(-40 * 0.49) per block.
constexpr double kCoherenceGate = 0.7;
// Largest per-block error the loop accepts while locked, in Hz. Anything
// bigger is a passing signal, not the beacon moving.
constexpr double kMaxLockedErrorHz = 60.0;
constexpr double kMaxReacquireErrorHz = 100.0;
// BPSK acquisition: a candidate whose plain (unsquared) spectrum peak
// stands this far above its 50..300 Hz surroundings is a carrier, not the
// beacon. Pure BPSK noise-like bins exceed 10 dB with odds ~1e-4.
constexpr double kCarrierRejectDb = 10.0;
// ...and whose 50..300 Hz surroundings do not stand this far above the
// window's plain noise floor has no BPSK signal under it.
constexpr double kBeaconHumpDb = 3.0;
// Peaks of one acquisition window tried in turn, strongest first.
constexpr size_t kMaxCandidates = 6;
constexpr size_t kMaxTries = 48;
// BPSK: a new lock is confirmed on ~1 s of the tracking chain's first
// stage (8 kS/s, 700 Hz around the candidate) before it is reported:
// - squared, it must show one line kConfirmDb above the band's noise,
//   within kConfirmSpanHz of the candidate (whose window may have been
//   swept seconds ago, with the LNB drifting since);
// - unsquared, its power, measured every 40 ms, must stay steady: the
//   beacon (and noise) does not change level, a voice comes and goes.
constexpr double kConfirmSeconds = 1.0;
constexpr double kConfirmSpanHz = 300.0;
constexpr double kConfirmDb = 15.0;
constexpr double kMaxEnvelopeSpread = 0.3;
// A beacon drifts a few Hz per second (a LNB without PLL warming up,
// maybe 20). A loop running faster follows something else: search again.
constexpr double kMaxDriftHzPerS = 150.0;
// Candidates closer than this are taken as the same signal (a voice
// station's squared spectrum is a cluster of lines; the beacon's one line
// spans a few Hz at most).
constexpr double kSameSignalHz = 1500.0;

size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

}  // namespace

const char* to_string(LockState s) {
    switch (s) {
        case LockState::Searching: return "searching";
        case LockState::Locked: return "locked";
        case LockState::Holdover: return "holdover";
    }
    return "?";
}

BeaconLock::BeaconLock(const BeaconLockConfig& cfg)
    : cfg_(cfg), acq_nco_(cfg.sample_rate), trk_nco_(cfg.sample_rate) {
    const double fs = cfg_.sample_rate;
    // Sub-window half width and overlap for wide searches.
    constexpr double kAcqHalfWidth = 25000.0, kAcqOverlap = 10000.0;
    const double w = std::min(cfg_.search_half_width_hz, kAcqHalfWidth);
    acq_half_ = w;
    acq_centres_ = {0.0};
    const double step = 2.0 * w - kAcqOverlap;
    for (int k = 1; (k - 1) * step + w < cfg_.search_half_width_hz && step > 0; ++k) {
        acq_centres_.push_back(k * step);
        acq_centres_.push_back(-k * step);
    }

    // Squaring doubles the offsets: keep +/- 2w inside the decimated band.
    unsigned acq_decim = static_cast<unsigned>(std::floor(fs / ((cfg_.bpsk ? 5.0 : 2.5) * w)));
    if (acq_decim < 1) acq_decim = 1;
    acq_rate_ = fs / acq_decim;
    acq_fir_ = make_decimator(fs, acq_decim, 1.05 * w);
    acq_fft_size_ = next_pow2(static_cast<size_t>(acq_rate_));  // ~1 s
    set_acq_window(0);

    unsigned d1 = static_cast<unsigned>(std::lround(fs / 8000.0));
    if (d1 < 1) d1 = 1;
    const double rate1 = fs / d1;
    trk_d1_ = d1;
    confirm_len_ = static_cast<size_t>(rate1 * kConfirmSeconds) + 256;
    env_block_ = static_cast<unsigned>(std::lround(rate1 * 0.04));
    unsigned d2 = static_cast<unsigned>(std::lround(rate1 / 1000.0));
    if (d2 < 1) d2 = 1;
    trk_rate_ = rate1 / d2;
    // BPSK: the first stage keeps the 400 Bd main lobe for the squarer.
    trk_fir1_ = make_decimator(fs, d1, cfg_.bpsk ? 700.0 : 300.0);
    trk_fir2_ = make_decimator(rate1, d2, 150.0);
    block_len_ = static_cast<unsigned>(std::lround(trk_rate_ * 0.04));
    if (block_len_ < 4) block_len_ = 4;

    const double loop_hz = cfg_.loop_natural_hz > 0.0 ? cfg_.loop_natural_hz
                                                     : (cfg_.bpsk ? 0.5 : 0.2);
    const double wn = 2.0 * kPi * loop_hz;
    const double dt = block_len_ / trk_rate_;
    k1_ = 2.0 * kZeta * wn * dt;
    k2_ = wn * wn * dt;

    freq_ = cfg_.beacon_offset_hz;
    good_freq_ = freq_;
}

void BeaconLock::process(const cf* iq, size_t n) {
    if (state_ == LockState::Searching) {
        acquire_block(iq, n);
    } else {
        track_block(iq, n);
    }
    samples_ += n;
}

void BeaconLock::acquire_block(const cf* iq, size_t n) {
    scratch_.resize(n);
    acq_nco_.mix_down(iq, scratch_.data(), n);
    acq_fir_.process(scratch_.data(), n, acq_buf_);
    if (acq_buf_.size() >= acq_fft_size_) run_acquisition();
}

void BeaconLock::set_acq_window(size_t index) {
    acq_index_ = index % acq_centres_.size();
    acq_nco_.set_frequency(cfg_.beacon_offset_hz + acq_centres_[acq_index_]);
}

void BeaconLock::run_acquisition() {
    const size_t before = sweep_.size();
    acquire_at(cfg_.beacon_offset_hz + acq_centres_[acq_index_], sweep_);
    if (sweep_.size() == before) sweep_snr_db_ = std::max(sweep_snr_db_, acq_snr_db_);
    // Wide search: every window gets its turn, then the clearest peak of
    // the sweep wins. Locking on the first one above the threshold could
    // take, in the centre window, the product of the beacon with its IQ
    // image (a line at the stream centre) while the beacon sits elsewhere.
    const bool last = acq_index_ + 1 == acq_centres_.size();
    if (acq_centres_.size() > 1) {
        set_acq_window(acq_index_ + 1);
        acq_buf_.clear();
        acq_fir_.reset();  // no samples mixed at the old frequency
    }
    if (!last) return;
    std::stable_sort(sweep_.begin(), sweep_.end(),
                     [](const Candidate& a, const Candidate& b) { return a.snr_db > b.snr_db; });
    // Peaks within kSameSignalHz of a stronger one belong to the same
    // signal (a voice station gives clusters of them): one try is enough.
    pending_.clear();
    for (const Candidate& c : sweep_) {
        bool seen = false;
        for (const Candidate& p : pending_) seen = seen || std::abs(p.freq_hz - c.freq_hz) < kSameSignalHz;
        if (!seen) pending_.push_back(c);
    }
    sweep_.clear();
    acq_snr_db_ = pending_.empty() ? sweep_snr_db_ : pending_.front().snr_db;
    sweep_snr_db_ = -100.0;
    lock_best();
}

void BeaconLock::lock_best() {
    if (pending_.empty()) {
        back_to_search();
        return;
    }
    const Candidate c = pending_.front();
    pending_.erase(pending_.begin());
    // If it fails, the peaks around it (same signal) go with it.
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const Candidate& p) { return std::abs(p.freq_hz - c.freq_hz) < kSameSignalHz; }),
                   pending_.end());
    acq_snr_db_ = c.snr_db;
    enter_tracking(c.freq_hz);
}

void BeaconLock::back_to_search() {
    // A rejected candidate is not reported: show the last real lock.
    if (verifying_) freq_ = good_freq_;
    state_ = LockState::Searching;
    verifying_ = false;
    acq_buf_.clear();
    acq_fir_.reset();
    set_acq_window(0);
}

void BeaconLock::acquire_at(double centre_hz, std::vector<Candidate>& out) {
    const size_t len = acq_fft_size_;
    std::vector<std::complex<double>> spec(len);
    std::vector<std::complex<double>> plain;  // BPSK: the unsquared spectrum too
    if (cfg_.bpsk) plain.resize(len);
    for (size_t i = 0; i < len; ++i) {
        const double hann = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (len - 1));
        std::complex<double> z(acq_buf_[i].real(), acq_buf_[i].imag());
        if (cfg_.bpsk) {
            plain[i] = z * hann;
            z *= z;
        }
        spec[i] = z * hann;
    }
    acq_buf_.clear();
    fft_inplace(spec);
    if (cfg_.bpsk) fft_inplace(plain);

    const double bin_hz = acq_rate_ / len;
    const double mult = cfg_.bpsk ? 2.0 : 1.0;  // squared: the line sits at 2 f
    const long half = static_cast<long>(mult * acq_half_ / bin_hz);
    std::vector<double> power;
    power.reserve(2 * half + 1);
    for (long k = -half; k <= half; ++k) {
        const size_t idx = static_cast<size_t>((k + static_cast<long>(len)) % static_cast<long>(len));
        power.push_back(std::norm(spec[idx]));
    }
    auto pw = [&](long k) { return power[static_cast<size_t>(k + half)]; };
    std::vector<double> sorted = power;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const double median = std::max(sorted[sorted.size() / 2], 1e-30);

    // Candidates: the strongest local maxima, strongest first. The stream's
    // own DC (LO leakage) is a carrier too and squares into a line: when it
    // falls in the window, it is never a candidate.
    const double dc_hz = -mult * centre_hz;
    std::vector<long> cands;
    for (long k = -half + 2; k <= half - 2; ++k) {
        if (std::abs(k * bin_hz - dc_hz) < cfg_.dc_guard_hz) continue;
        const double p = pw(k);
        if (p < pw(k - 1) || p < pw(k + 1) || p < pw(k - 2) || p < pw(k + 2)) continue;
        if (10.0 * std::log10(p / median) < cfg_.acquisition_snr_db) continue;
        cands.push_back(k);
    }
    std::sort(cands.begin(), cands.end(), [&](long a, long b) { return pw(a) > pw(b); });
    // BPSK: a loud station elsewhere leaves many lines (squaring products,
    // aliases) that the plain-spectrum checks throw away; look further
    // down the list for the beacon, still keeping kMaxCandidates at most.
    const size_t max_tries = cfg_.bpsk ? kMaxTries : kMaxCandidates;
    if (cands.size() > max_tries) cands.resize(max_tries);
    size_t kept = 0;
    acq_snr_db_ = cands.empty() ? 10.0 * std::log10(*std::max_element(power.begin(), power.end()) / median)
                                : 10.0 * std::log10(pw(cands[0]) / median);

    const long edge = static_cast<long>(std::ceil(mult * 1000.0 / bin_hz));
    for (const long best : cands) {
        const double best_p = pw(best);
        // A peak against the window edge is most likely something else (a
        // station, the filter skirt) while the beacon lies outside: keep
        // searching rather than lock on what comes next.
        // (BPSK: skip it, every lock is verified anyway.)
        if (std::abs(best) >= half - edge) {
            if (cfg_.bpsk) continue;
            return;
        }

        // IQ imbalance mirrors the beacon to the other side of the stream
        // centre: at x Hz from the expected position it sits at -2*offset - x,
        // and in this (possibly squared) spectrum at -2*mult*offset - k*bin.
        // When that is in the window, the peak must clearly beat it, or the
        // search waits (just after tuning, the image can be the stronger).
        const long mirror = std::lround((-2.0 * mult * centre_hz) / bin_hz) - best;
        if (std::abs(mirror - best) > 2 && std::abs(mirror) + 1 < half) {
            double mirror_p = 0.0;
            for (long k = mirror - 1; k <= mirror + 1; ++k) mirror_p = std::max(mirror_p, pw(k));
            const double margin_db = mult * cfg_.image_margin_db;
            if (10.0 * std::log10(best_p / std::max(mirror_p, 1e-30)) < margin_db) return;  // ambiguous: wait
        }

        // Parabolic interpolation on log power for sub-bin precision.
        double delta = 0.0;
        const long i = best + half;
        if (i > 0 && i + 1 < static_cast<long>(power.size())) {
            const double a = std::log(power[i - 1] + 1e-30);
            const double b = std::log(power[i] + 1e-30);
            const double c = std::log(power[i + 1] + 1e-30);
            const double den = a - 2.0 * b + c;
            if (den != 0.0) delta = std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
        }
        const double rel_hz = (best + delta) * bin_hz / mult;

        // The BPSK beacon has no carrier: its plain spectrum is a hump some
        // 800 Hz wide. A steady carrier (a spur of the board, an unmodulated
        // station) squares into a line as clean as the beacon's, or cleaner,
        // and once won the sweep 40 kHz off (FishBall, LNB +24.7 kHz). A
        // candidate that is a line in the plain spectrum is not the beacon,
        // and neither is one with no plain signal under it (a product of
        // two other signals, such as the spur and the beacon).
        if (cfg_.bpsk && !looks_like_bpsk(plain, rel_hz, bin_hz)) continue;

        out.push_back({10.0 * std::log10(best_p / median), centre_hz + rel_hz});
        // CW: the strongest clear peak is the beacon. BPSK: the others are
        // kept in case it turns out not to be a steady line.
        if (!cfg_.bpsk || ++kept == kMaxCandidates) return;
    }
}

bool BeaconLock::looks_like_bpsk(const std::vector<std::complex<double>>& plain, double rel_hz, double bin_hz) const {
    const long len = static_cast<long>(plain.size());
    auto at = [&](long k) { return std::norm(plain[static_cast<size_t>(((k % len) + len) % len)]); };
    const long c = std::lround(rel_hz / bin_hz);
    double peak = 0.0;
    for (long k = c - 2; k <= c + 2; ++k) peak = std::max(peak, at(k));
    double sum = 0.0;
    long count = 0;
    const long near = std::max(3L, static_cast<long>(std::ceil(50.0 / bin_hz)));
    const long far = std::max(near + 8, static_cast<long>(std::ceil(300.0 / bin_hz)));
    for (long d = near; d <= far; ++d) {
        sum += at(c - d) + at(c + d);
        count += 2;
    }
    const double around = std::max(sum / count, 1e-30);
    if (10.0 * std::log10(peak / around) > kCarrierRejectDb) return false;  // a carrier
    const long whalf = static_cast<long>(acq_half_ / bin_hz);
    std::vector<double> floor_p;
    floor_p.reserve(2 * whalf + 1);
    for (long k = -whalf; k <= whalf; ++k) floor_p.push_back(at(k));
    std::nth_element(floor_p.begin(), floor_p.begin() + floor_p.size() / 2, floor_p.end());
    const double plain_median = std::max(floor_p[floor_p.size() / 2], 1e-30);
    return 10.0 * std::log10(around / plain_median) >= kBeaconHumpDb;  // a signal is there
}

void BeaconLock::enter_tracking(double beacon_freq_hz) {
    freq_ = beacon_freq_hz;
    rate_ = 0.0;
    trk_nco_.reset();
    trk_nco_.set_frequency(freq_);
    trk_fir1_.reset();
    trk_fir2_.reset();
    prev_ = cf(0.0f, 0.0f);
    prod_ = {0.0, 0.0};
    prod_mag_ = 0.0;
    block_count_ = 0;
    last_carrier_sample_ = samples_;
    state_ = LockState::Locked;
    verifying_ = cfg_.bpsk;
    confirm_buf_.clear();
    confirm_env_.clear();
    env_acc_ = 0.0;
    env_count_ = 0;
}

void BeaconLock::confirm() {
    const double rate1 = cfg_.sample_rate / trk_d1_;
    size_t len = 1;
    while (len < confirm_buf_.size()) len <<= 1;
    len >>= 1;  // the newest power of two that fits
    std::vector<std::complex<double>> spec(len);
    const size_t skip = confirm_buf_.size() - len;  // drop the filter's start
    for (size_t i = 0; i < len; ++i) {
        const double hann = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (len - 1));
        const cf z = confirm_buf_[skip + i];
        spec[i] = std::complex<double>(z.real(), z.imag()) * hann;
    }
    confirm_buf_.clear();
    fft_inplace(spec);
    const double bin_hz = rate1 / len;
    auto pw = [&](long k) { return std::norm(spec[static_cast<size_t>((k + static_cast<long>(len)) % static_cast<long>(len))]); };
    // Noise: the squared 700 Hz band (it spans twice that).
    const long nb = static_cast<long>(1200.0 / bin_hz);
    std::vector<double> noise;
    for (long k = -nb; k <= nb; ++k) noise.push_back(pw(k));
    std::nth_element(noise.begin(), noise.begin() + noise.size() / 2, noise.end());
    const double median = std::max(noise[noise.size() / 2], 1e-30);
    const long span = static_cast<long>(std::ceil(2.0 * kConfirmSpanHz / bin_hz));
    long best = 0;
    for (long k = -span; k <= span; ++k)
        if (pw(k) > pw(best)) best = k;
    const double snr_db = 10.0 * std::log10(pw(best) / median);

    // Envelope: spread (std / mean) of the 40 ms block powers.
    double sum = 0.0, sum2 = 0.0;
    for (const double p : confirm_env_) {
        sum += p;
        sum2 += p * p;
    }
    const double nblk = static_cast<double>(confirm_env_.size());
    const double mean = nblk > 0 ? sum / nblk : 0.0;
    const double spread = mean > 0 ? std::sqrt(std::max(0.0, sum2 / nblk - mean * mean)) / mean : 1e9;
    confirm_env_.clear();
    if (snr_db < kConfirmDb || spread > kMaxEnvelopeSpread) {
        lock_best();  // not the beacon: next candidate, or a new sweep
        return;
    }
    double delta = 0.0;
    const double a = std::log(pw(best - 1) + 1e-30), b = std::log(pw(best) + 1e-30), c = std::log(pw(best + 1) + 1e-30);
    if (a - 2.0 * b + c != 0.0) delta = std::clamp(0.5 * (a - c) / (a - 2.0 * b + c), -0.5, 0.5);
    freq_ += (best + delta) * bin_hz / 2.0;
    trk_nco_.set_frequency(freq_);
    trk_fir1_.reset();
    trk_fir2_.reset();
    prev_ = cf(0.0f, 0.0f);
    verifying_ = false;
    good_freq_ = freq_;
    last_carrier_sample_ = samples_;
}

void BeaconLock::track_block(const cf* iq, size_t n) {
    scratch_.resize(n);
    trk_nco_.mix_down(iq, scratch_.data(), n);
    stage1_.clear();
    trk_fir1_.process(scratch_.data(), n, stage1_);
    if (verifying_) {
        for (const cf& z : stage1_) {
            env_acc_ += std::norm(z);
            if (++env_count_ == env_block_) {
                confirm_env_.push_back(env_acc_ / env_count_);
                env_acc_ = 0.0;
                env_count_ = 0;
            }
            confirm_buf_.push_back(z * z);
        }
        if (confirm_buf_.size() >= confirm_len_) confirm();
        return;
    }
    if (cfg_.bpsk) {
        for (cf& z : stage1_) z *= z;
    }
    stage2_.clear();
    trk_fir2_.process(stage1_.data(), stage1_.size(), stage2_);


    for (const cf& z : stage2_) {
        const std::complex<double> zd(z.real(), z.imag());
        const std::complex<double> pd(prev_.real(), prev_.imag());
        prod_ += zd * std::conj(pd);
        prod_mag_ += std::abs(zd) * std::abs(pd);
        prev_ = z;
        if (++block_count_ < block_len_) continue;

        const double dt = block_len_ / trk_rate_;
        const bool coherent = prod_mag_ > 0.0 && std::abs(prod_) / prod_mag_ > kCoherenceGate;
        if (coherent) {
            double err = std::arg(prod_) * trk_rate_ / (2.0 * kPi);
            if (cfg_.bpsk) err *= 0.5;
            const double limit = (state_ == LockState::Locked) ? kMaxLockedErrorHz : kMaxReacquireErrorHz;
            if (std::abs(err) <= limit) {
                loop_update(err);
                last_carrier_sample_ = samples_;
            }
        }
        coast(dt);
        prod_ = {0.0, 0.0};
        prod_mag_ = 0.0;
        block_count_ = 0;

        if (std::abs(rate_) > kMaxDriftHzPerS) {
            pending_.clear();
            verifying_ = true;  // what it followed was not the beacon: do not report it
            back_to_search();
            return;
        }
        if (!verifying_) good_freq_ = freq_;

        const double quiet = static_cast<double>(samples_ - last_carrier_sample_) / cfg_.sample_rate;
        if (quiet > cfg_.reacquire_after_s) {
            pending_.clear();
            back_to_search();  // freq_ keeps the last estimate for reporting
            return;
        }
        state_ = quiet > cfg_.holdover_after_s ? LockState::Holdover : LockState::Locked;
    }
}

void BeaconLock::loop_update(double error_hz) {
    freq_ += k1_ * error_hz;
    rate_ += k2_ * error_hz;
}

void BeaconLock::coast(double dt) {
    freq_ += rate_ * dt;
    trk_nco_.set_frequency(freq_);
}

BeaconLockStatus BeaconLock::status() const {
    BeaconLockStatus s;
    s.state = state();
    s.correction_hz = correction_hz();
    s.drift_hz_per_s = rate_;
    s.acquisition_snr_db = acq_snr_db_;
    s.seconds_since_carrier = static_cast<double>(samples_ - last_carrier_sample_) / cfg_.sample_rate;
    return s;
}

}  // namespace nb
