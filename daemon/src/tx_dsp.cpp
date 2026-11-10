#include "tx_dsp.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <stdexcept>

namespace nb {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr float kInScale = 4096.0f;  // float +/-1 -> integer CIC input (13 bits)

int16_t diff16(uint16_t a, uint16_t b) { return static_cast<int16_t>(static_cast<uint16_t>(a - b)); }
}  // namespace

// ---------------------------------------------------------------- CIC

CicInterpolator::CicInterpolator(unsigned ratio, unsigned order) : ratio_(ratio), order_(order) {
    if (ratio < 1 || order < 1 || order > 6) throw std::invalid_argument("CIC: bad ratio or order");
    gain_ = std::pow(static_cast<double>(ratio), static_cast<double>(order - 1));
    // Output must fit in 31 bits for an input of 13 bits.
    if (gain_ * kInScale > 1073741824.0) throw std::invalid_argument("CIC: gain too large for 32-bit integrators");
    reset();
}

void CicInterpolator::reset() {
    comb_i_.assign(order_, 0);
    comb_q_.assign(order_, 0);
    integ_i_.assign(order_, 0);
    integ_q_.assign(order_, 0);
}

void CicInterpolator::comb(const int32_t* iq, size_t n, int32_t* out) {
    for (size_t k = 0; k < n; ++k) {
        uint32_t yi = static_cast<uint32_t>(iq[2 * k]);
        uint32_t yq = static_cast<uint32_t>(iq[2 * k + 1]);
        for (unsigned s = 0; s < order_; ++s) {
            const uint32_t ti = yi, tq = yq;
            yi -= comb_i_[s];
            yq -= comb_q_[s];
            comb_i_[s] = ti;
            comb_q_[s] = tq;
        }
        out[2 * k] = static_cast<int32_t>(yi);
        out[2 * k + 1] = static_cast<int32_t>(yq);
    }
}

void CicInterpolator::process(const int32_t* iq, size_t n, std::vector<int32_t>& out) {
    const size_t base = out.size();
    out.resize(base + 2 * n * ratio_);
    int32_t* o = out.data() + base;
    uint32_t* ci = comb_i_.data();
    uint32_t* cq = comb_q_.data();
    uint32_t* ii = integ_i_.data();
    uint32_t* iq_ = integ_q_.data();
    for (size_t k = 0; k < n; ++k) {
        uint32_t yi = static_cast<uint32_t>(iq[2 * k]);
        uint32_t yq = static_cast<uint32_t>(iq[2 * k + 1]);
        for (unsigned s = 0; s < order_; ++s) {
            const uint32_t ti = yi, tq = yq;
            yi -= ci[s];
            yq -= cq[s];
            ci[s] = ti;
            cq[s] = tq;
        }
        for (unsigned r = 0; r < ratio_; ++r) {
            uint32_t vi = r == 0 ? yi : 0u;
            uint32_t vq = r == 0 ? yq : 0u;
            for (unsigned s = 0; s < order_; ++s) {
                ii[s] += vi;
                vi = ii[s];
                iq_[s] += vq;
                vq = iq_[s];
            }
            *o++ = static_cast<int32_t>(vi);
            *o++ = static_cast<int32_t>(vq);
        }
    }
}

// ---------------------------------------------------------------- upconverter

Upconverter::Upconverter(double out_rate, double full_scale)
    : out_rate_(out_rate),
      full_scale_(full_scale),
      cic_(static_cast<unsigned>(std::lround(out_rate / kInRate)), 4) {
    const double r = out_rate / kInRate;
    if (r < 2.0 || std::fabs(r - std::round(r)) > 1e-3)
        throw std::invalid_argument("Upconverter: the DAC rate must be a multiple of 48 kHz");
    if (cic_.order() != 4) throw std::invalid_argument("Upconverter: the fused loop is for a 4th order CIC");
    // Full scale at the last integrator, brought down to at most 16 bits.
    const double top = kInScale * cic_.gain();
    while (top / std::ldexp(1.0, static_cast<int>(pre_shift_)) > 65536.0) ++pre_shift_;
    const double a_full = top / std::ldexp(1.0, static_cast<int>(pre_shift_));
    // out = (a * table) >> 14, with full_scale * 32767 at a = a_full.
    const double amp = full_scale_ * 32767.0 * 16384.0 / a_full;
    if (amp > 32767.0) throw std::invalid_argument("Upconverter: NCO table overflow");
    const size_t n = size_t{1} << kNcoBits;
    nco_.resize(2 * n);
    for (size_t i = 0; i < n; ++i) {
        const double ph = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n);
        nco_[2 * i] = static_cast<int16_t>(std::lrint(amp * std::cos(ph)));
        nco_[2 * i + 1] = static_cast<int16_t>(std::lrint(amp * std::sin(ph)));
    }
}

void Upconverter::set_offset_hz(double hz) {
    if (!(std::fabs(hz) < 0.45 * out_rate_)) throw std::invalid_argument("Upconverter: offset outside the DAC band");
    offset_ = hz;
    const double c = hz / out_rate_;  // cycles per sample, |c| < 0.45
    inc_ = static_cast<uint32_t>(static_cast<int64_t>(std::llround(c * 4294967296.0)));
}

void Upconverter::reset() {
    cic_.reset();
    phase_ = 0;
}

void Upconverter::process(const cf* in, size_t n, std::vector<int16_t>& out) {
    // Fused CIC (order 4) + NCO + int16, one pass at the DAC rate and all in
    // integers: this loop is the transmitter's main cost on the Cortex-A9.
    const unsigned R = cic_.ratio();
    in_i32_.resize(2 * n);
    for (size_t k = 0; k < n; ++k) {
        const float re = std::clamp(in[k].real(), -1.0f, 1.0f);
        const float im = std::clamp(in[k].imag(), -1.0f, 1.0f);
        in_i32_[2 * k] = static_cast<int32_t>(std::lrint(re * kInScale));
        in_i32_[2 * k + 1] = static_cast<int32_t>(std::lrint(im * kInScale));
    }
    // Comb section at the input rate.
    wide_.resize(2 * n);
    cic_.comb(in_i32_.data(), n, wide_.data());

    const size_t base = out.size();
    out.resize(base + 2 * n * R);
    int16_t* o = out.data() + base;
    const int16_t* tab = nco_.data();
    const unsigned sh = pre_shift_;
    constexpr unsigned kIdxShift = 32 - kNcoBits;
    uint32_t* gi = cic_.integ_i();
    uint32_t* gq = cic_.integ_q();
    uint32_t i0 = gi[0], i1 = gi[1], i2 = gi[2], i3 = gi[3];
    uint32_t q0 = gq[0], q1 = gq[1], q2 = gq[2], q3 = gq[3];
    uint32_t ph = phase_;
    const uint32_t inc = inc_;
    for (size_t j = 0; j < n; ++j) {
        i0 += static_cast<uint32_t>(wide_[2 * j]);
        q0 += static_cast<uint32_t>(wide_[2 * j + 1]);
        for (unsigned r = 0; r < R; ++r) {
            // The comb output enters once per input sample (above), then
            // the first integrator holds it: same as feeding zeros.
            i1 += i0;
            q1 += q0;
            i2 += i1;
            q2 += q1;
            i3 += i2;
            q3 += q2;
            const int32_t a = static_cast<int32_t>(i3) >> sh;
            const int32_t b = static_cast<int32_t>(q3) >> sh;
            const int16_t* cs = tab + 2 * (ph >> kIdxShift);
            ph += inc;
            const int32_t c = cs[0], s = cs[1];
            // |a|, |b| <= 2^16 and |c|, |s| < 2^15 * full_scale / 2: the
            // sums stay inside 32 bits.
            *o++ = static_cast<int16_t>((a * c - b * s + 8192) >> 14);
            *o++ = static_cast<int16_t>((a * s + b * c + 8192) >> 14);
        }
    }
    phase_ = ph;
    gi[0] = i0, gi[1] = i1, gi[2] = i2, gi[3] = i3;
    gq[0] = q0, gq[1] = q1, gq[2] = q2, gq[3] = q3;
}

// ---------------------------------------------------------------- jitter buffer

void PacketJitter::push(uint16_t seq, std::string packet) {
    std::lock_guard<std::mutex> lk(mu_);
    if (have_next_ && diff16(seq, next_) < 0) return;  // late: already played or concealed
    auto it = q_.end();
    while (it != q_.begin() && diff16(std::prev(it)->first, seq) > 0) --it;
    if (it != q_.begin() && std::prev(it)->first == seq) return;  // duplicate
    q_.insert(it, {seq, std::move(packet)});
    while (q_.size() > max_) {
        q_.pop_front();
        ++dropped_;
        if (have_next_) next_ = q_.front().first;
    }
}

PacketJitter::Pop PacketJitter::pop(std::string& packet) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!started_) {
        if (q_.size() < prefill_) return Pop::Empty;
        started_ = true;
        if (!have_next_ || diff16(q_.front().first, next_) < 0 || diff16(q_.front().first, next_) > 8) {
            next_ = q_.front().first;
        }
        have_next_ = true;
    }
    if (q_.empty()) {
        if (stretch_run_ == 0) ++underruns_;
        if (++stretch_run_ <= max_stretch_) return Pop::Stretch;
        stretch_run_ = 0;
        started_ = false;
        return Pop::Empty;
    }
    stretch_run_ = 0;
    if (q_.front().first == next_) {
        packet = std::move(q_.front().second);
        q_.pop_front();
        ++next_;
        return Pop::Packet;
    }
    ++next_;  // the expected one never came: conceal it
    return Pop::Missing;
}

void PacketJitter::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    q_.clear();
    started_ = false;
    stretch_run_ = 0;
    have_next_ = false;
}

size_t PacketJitter::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return q_.size();
}

// ---------------------------------------------------------------- TX calibration

TxCalibrator::TxCalibrator(double rate, size_t fft_size, double settle_s)
    : rate_(rate), settle_s_(settle_s), n_(fft_size) {
    if (fft_size < 1024 || (fft_size & (fft_size - 1)) != 0) throw std::invalid_argument("TxCalibrator: FFT size");
}

void TxCalibrator::start(double expected_hz, double cal_hz) {
    expected_ = expected_hz;
    cal_ = cal_hz;
    waited_ = 0;
    buf_.clear();
    buf_.reserve(n_);
    msg_.clear();
    step_ = Step::Running;
}

TxCalibrator::Step TxCalibrator::feed(const cf* x, size_t n, double block_s, bool carrier_on, bool locked) {
    if (step_ != Step::Running) return step_ == Step::Idle ? Step::Idle : step_;
    if (!carrier_on) return finish(Step::Failed, "calibration needs the tune carrier on the air");
    if (!locked) return finish(Step::Failed, "calibration needs the beacon locked");
    // The carrier needs the round trip to the satellite and the filters to
    // settle before it can be measured.
    if (waited_ < settle_s_) {
        waited_ += block_s;
        return Step::Running;
    }
    for (size_t i = 0; i < n && buf_.size() < n_; ++i) buf_.push_back(x[i]);
    if (buf_.size() < n_) return Step::Running;

    // Hann window, FFT, strongest bin around the expected tone, parabolic
    // interpolation on the log magnitude (a fraction of a bin: < 0.1 Hz).
    std::vector<std::complex<double>> a(n_);
    for (size_t i = 0; i < n_; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n_));
        a[i] = std::complex<double>(buf_[i].real(), buf_[i].imag()) * w;
    }
    fft_inplace(a);
    const double df = rate_ / static_cast<double>(n_);
    auto bin_of = [&](double f) {
        long k = std::lround(f / df);
        return static_cast<size_t>((k % static_cast<long>(n_) + static_cast<long>(n_)) % static_cast<long>(n_));
    };
    auto pw = [&](size_t k) { return std::norm(a[k % n_]) + 1e-30; };
    const long lo = std::lround((expected_ - kMaxStepHz) / df), hi = std::lround((expected_ + kMaxStepHz) / df);
    long best = lo;
    double best_p = 0;
    std::vector<double> all;
    all.reserve(static_cast<size_t>(hi - lo + 1));
    for (long k = lo; k <= hi; ++k) {
        const double p = pw(bin_of(static_cast<double>(k) * df));
        all.push_back(p);
        if (p > best_p) best_p = p, best = k;
    }
    std::nth_element(all.begin(), all.begin() + static_cast<long>(all.size() / 2), all.end());
    const double floor_p = all[all.size() / 2];
    snr_ = 10.0 * std::log10(best_p / floor_p);
    if (snr_ < 20.0) return finish(Step::Failed, "own carrier not found near the dial (SNR " + std::to_string(static_cast<int>(snr_)) + " dB)");
    const double ym = std::log(pw(bin_of(static_cast<double>(best - 1) * df)));
    const double y0 = std::log(best_p);
    const double yp = std::log(pw(bin_of(static_cast<double>(best + 1) * df)));
    const double den = ym - 2.0 * y0 + yp;
    const double frac = den < 0 ? std::clamp(0.5 * (ym - yp) / den, -0.5, 0.5) : 0.0;
    measured_ = (static_cast<double>(best) + frac) * df;
    new_cal_ = cal_ + (expected_ - measured_);
    if (std::fabs(new_cal_) > kMaxCalHz) {
        char m[120];
        std::snprintf(m, sizeof m, "TX correction %+.0f Hz out of range: wrong carrier?", new_cal_);
        return finish(Step::Failed, m);
    }
    char m[120];
    std::snprintf(m, sizeof m, "carrier at %+.1f Hz, TX correction %+.0f Hz", measured_, new_cal_);
    return finish(Step::Done, m);
}

}  // namespace nb
