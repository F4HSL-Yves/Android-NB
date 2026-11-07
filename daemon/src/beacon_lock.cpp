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
// A step larger than kMaxLockedErrorHz (the AD9363 reference being trimmed,
// an LNB jump) is accepted at once when this many coherent blocks in a row
// agree on it within kStepAgreeHz: a carrier that moved, not an interferer.
constexpr unsigned kStepBlocks = 5;  // 200 ms
constexpr double kStepAgreeHz = 8.0;

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
    const double w = cfg_.search_half_width_hz;

    unsigned acq_decim = static_cast<unsigned>(std::floor(fs / (2.5 * w)));
    if (acq_decim < 1) acq_decim = 1;
    acq_rate_ = fs / acq_decim;
    acq_fir_ = make_decimator(fs, acq_decim, 1.05 * w);
    acq_fft_size_ = next_pow2(static_cast<size_t>(acq_rate_));  // ~1 s
    acq_nco_.set_frequency(cfg_.beacon_offset_hz);

    unsigned d1 = static_cast<unsigned>(std::lround(fs / 8000.0));
    if (d1 < 1) d1 = 1;
    const double rate1 = fs / d1;
    unsigned d2 = static_cast<unsigned>(std::lround(rate1 / 1000.0));
    if (d2 < 1) d2 = 1;
    trk_rate_ = rate1 / d2;
    trk_fir1_ = make_decimator(fs, d1, 300.0);
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

void BeaconLock::run_acquisition() {
    const size_t len = acq_fft_size_;
    std::vector<std::complex<double>> spec(len);
    for (size_t i = 0; i < len; ++i) {
        const double hann = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (len - 1));
        spec[i] = std::complex<double>(acq_buf_[i].real(), acq_buf_[i].imag()) * hann;
    }
    acq_buf_.clear();
    fft_inplace(spec);

    const double bin_hz = acq_rate_ / len;
    const long half = static_cast<long>(cfg_.search_half_width_hz / bin_hz);
    std::vector<double> power;
    power.reserve(2 * half + 1);
    long best = 0;
    double best_p = -1.0;
    for (long k = -half; k <= half; ++k) {
        const size_t idx = static_cast<size_t>((k + static_cast<long>(len)) % static_cast<long>(len));
        const double p = std::norm(spec[idx]);
        power.push_back(p);
        if (p > best_p) {
            best_p = p;
            best = k;
        }
    }
    std::vector<double> sorted = power;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const double median = std::max(sorted[sorted.size() / 2], 1e-30);
    acq_snr_db_ = 10.0 * std::log10(best_p / median);
    if (acq_snr_db_ < cfg_.acquisition_snr_db) return;  // keep searching

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
    enter_tracking(cfg_.beacon_offset_hz + (best + delta) * bin_hz);
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
    step_count_ = 0;
    last_carrier_sample_ = samples_;
    state_ = LockState::Locked;
}

void BeaconLock::track_block(const cf* iq, size_t n) {
    scratch_.resize(n);
    trk_nco_.mix_down(iq, scratch_.data(), n);
    stage1_.clear();
    trk_fir1_.process(scratch_.data(), n, stage1_);
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
            const double err = std::arg(prod_) * trk_rate_ / (2.0 * kPi);
            const double limit = (state_ == LockState::Locked) ? kMaxLockedErrorHz : kMaxReacquireErrorHz;
            if (std::abs(err) <= limit) {
                loop_update(err);
                last_carrier_sample_ = samples_;
                step_count_ = 0;
            } else if (std::abs(err) <= kMaxReacquireErrorHz) {
                if (step_count_ > 0 && std::abs(err - step_sum_ / step_count_) > kStepAgreeHz) step_count_ = 0;
                if (step_count_ == 0) step_sum_ = 0.0;
                step_sum_ += err;
                if (++step_count_ >= kStepBlocks) {
                    freq_ += step_sum_ / step_count_;  // jump; the loop keeps its drift rate
                    last_carrier_sample_ = samples_;
                    step_count_ = 0;
                }
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
