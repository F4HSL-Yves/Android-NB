#include "tx_dsp.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace nb {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr float kInScale = 4096.0f;  // float +/-1 -> integer CIC input (13 bits)
constexpr size_t kNcoBlock = 64;     // exact phase recomputed every kNcoBlock samples

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
}

void Upconverter::set_offset_hz(double hz) {
    if (!(std::fabs(hz) < 0.45 * out_rate_)) throw std::invalid_argument("Upconverter: offset outside the DAC band");
    offset_ = hz;
}

void Upconverter::reset() {
    cic_.reset();
    phase_ = 0;
}

void Upconverter::process(const cf* in, size_t n, std::vector<int16_t>& out) {
    // Fused CIC (order 4) + NCO + int16, one pass at the DAC rate: this loop
    // is the transmitter's main cost on the Cortex-A9.
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
    const float k = static_cast<float>(full_scale_ * 32767.0 / (kInScale * cic_.gain()));
    const double cycles_per_sample = offset_ / out_rate_;
    const double w = 2.0 * kPi * cycles_per_sample;
    const float sc = static_cast<float>(std::cos(w)), ss = static_cast<float>(std::sin(w));
    uint32_t* gi = cic_.integ_i();
    uint32_t* gq = cic_.integ_q();
    uint32_t i0 = gi[0], i1 = gi[1], i2 = gi[2], i3 = gi[3];
    uint32_t q0 = gq[0], q1 = gq[1], q2 = gq[2], q3 = gq[3];
    float rc = 1, rs = 0;
    size_t until_resync = 0;
    for (size_t j = 0; j < n; ++j) {
        const uint32_t xi = static_cast<uint32_t>(wide_[2 * j]);
        const uint32_t xq = static_cast<uint32_t>(wide_[2 * j + 1]);
        for (unsigned r = 0; r < R; ++r) {
            if (until_resync == 0) {
                // Exact phase every kNcoBlock samples; the float rotator in between.
                rc = static_cast<float>(std::cos(2.0 * kPi * phase_));
                rs = static_cast<float>(std::sin(2.0 * kPi * phase_));
                phase_ += cycles_per_sample * static_cast<double>(kNcoBlock);
                phase_ -= std::floor(phase_);
                until_resync = kNcoBlock;
            }
            --until_resync;
            i0 += r == 0 ? xi : 0u;
            q0 += r == 0 ? xq : 0u;
            i1 += i0;
            q1 += q0;
            i2 += i1;
            q2 += q1;
            i3 += i2;
            q3 += q2;
            const float fi = static_cast<float>(static_cast<int32_t>(i3)) * k;
            const float fq = static_cast<float>(static_cast<int32_t>(q3)) * k;
            const float yi = fi * rc - fq * rs;
            const float yq = fi * rs + fq * rc;
            // |y| <= full_scale * 32767: no clamp needed, round to nearest.
            *o++ = static_cast<int16_t>(yi + (yi >= 0 ? 0.5f : -0.5f));
            *o++ = static_cast<int16_t>(yq + (yq >= 0 ? 0.5f : -0.5f));
            const float nc = rc * sc - rs * ss;
            rs = rc * ss + rs * sc;
            rc = nc;
        }
    }
    // Phase of the next sample: the resync above ran ahead by until_resync.
    phase_ -= cycles_per_sample * static_cast<double>(until_resync);
    phase_ -= std::floor(phase_);
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
        started_ = false;
        ++underruns_;
        return Pop::Empty;
    }
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
    have_next_ = false;
}

size_t PacketJitter::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return q_.size();
}

}  // namespace nb
