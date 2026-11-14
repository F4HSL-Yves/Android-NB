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
constexpr double kMaxLockedErrorHz = 25.0;
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
    unsigned d2 = static_cast<unsigned>(std::lround(rate1 / 1000.0));
    if (d2 < 1) d2 = 1;
    trk_rate_ = rate1 / d2;
    // BPSK: the first stage keeps the 400 Bd main lobe for the squarer.
    trk_fir1_ = make_decimator(fs, d1, cfg_.bpsk ? 700.0 : 300.0);
    trk_fir2_ = make_decimator(rate1, d2, 150.0);
    block_len_ = static_cast<unsigned>(std::lround(trk_rate_ * 0.04));
    if (block_len_ < 4) block_len_ = 4;

    const double wn = 2.0 * kPi * cfg_.loop_natural_hz;
    const double dt = block_len_ / trk_rate_;
    k1_ = 2.0 * kZeta * wn * dt;
    k2_ = wn * wn * dt;

    freq_ = cfg_.beacon_offset_hz;
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
    double freq = 0.0;
    const bool found = acquire_at(cfg_.beacon_offset_hz + acq_centres_[acq_index_], freq);
    if (acq_centres_.size() == 1) {
        if (found) enter_tracking(freq);
        return;
    }
    // Wide search: every window gets its turn, then the clearest peak of
    // the sweep wins. Locking on the first one above the threshold could
    // take, in the centre window, the product of the beacon with its IQ
    // image (a line at the stream centre) while the beacon sits elsewhere.
    if (found && (!sweep_found_ || acq_snr_db_ > sweep_snr_db_)) {
        sweep_found_ = true;
        sweep_snr_db_ = acq_snr_db_;
        sweep_freq_ = freq;
    }
    if (!sweep_found_) sweep_snr_db_ = std::max(sweep_snr_db_, acq_snr_db_);
    const bool last = acq_index_ + 1 == acq_centres_.size();
    set_acq_window(acq_index_ + 1);
    acq_buf_.clear();
    acq_fir_.reset();  // no samples mixed at the old frequency
    if (!last) return;
    acq_snr_db_ = sweep_snr_db_;
    if (sweep_found_) enter_tracking(sweep_freq_);
    sweep_found_ = false;
    sweep_snr_db_ = -100.0;
}

bool BeaconLock::acquire_at(double centre_hz, double& freq_hz) {
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
    if (cands.size() > kMaxCandidates) cands.resize(kMaxCandidates);
    acq_snr_db_ = cands.empty() ? 10.0 * std::log10(*std::max_element(power.begin(), power.end()) / median)
                                : 10.0 * std::log10(pw(cands[0]) / median);

    const long edge = static_cast<long>(std::ceil(mult * 1000.0 / bin_hz));
    for (const long best : cands) {
        const double best_p = pw(best);
        // A peak against the window edge is most likely something else (a
        // station, the filter skirt) while the beacon lies outside: keep
        // searching rather than lock on what comes next.
        if (std::abs(best) >= half - edge) return false;

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
            if (10.0 * std::log10(best_p / std::max(mirror_p, 1e-30)) < margin_db) return false;  // ambiguous: wait
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

        acq_snr_db_ = 10.0 * std::log10(best_p / median);
        freq_hz = centre_hz + rel_hz;
        return true;
    }
    return false;  // keep searching
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
}

void BeaconLock::track_block(const cf* iq, size_t n) {
    scratch_.resize(n);
    trk_nco_.mix_down(iq, scratch_.data(), n);
    stage1_.clear();
    trk_fir1_.process(scratch_.data(), n, stage1_);
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

        const double quiet = static_cast<double>(samples_ - last_carrier_sample_) / cfg_.sample_rate;
        if (quiet > cfg_.reacquire_after_s) {
            state_ = LockState::Searching;
            acq_buf_.clear();
            acq_fir_.reset();
            set_acq_window(0);
            freq_ = cfg_.beacon_offset_hz + correction_hz();  // keep last estimate for reporting
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
    s.state = state_;
    s.correction_hz = correction_hz();
    s.drift_hz_per_s = rate_;
    s.acquisition_snr_db = acq_snr_db_;
    s.seconds_since_carrier = static_cast<double>(samples_ - last_carrier_sample_) / cfg_.sample_rate;
    return s;
}

}  // namespace nb
