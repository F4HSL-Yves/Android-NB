#include "spectrum.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace nb {

namespace {
constexpr double kPi = 3.14159265358979323846;

void put_u16(std::string& s, uint16_t v) {
    s.push_back(static_cast<char>(v >> 8));
    s.push_back(static_cast<char>(v & 0xFF));
}
void put_u32(std::string& s, uint32_t v) {
    put_u16(s, static_cast<uint16_t>(v >> 16));
    put_u16(s, static_cast<uint16_t>(v & 0xFFFF));
}
}  // namespace

Fft::Fft(size_t size) : n_(size) {
    if (n_ < 2 || (n_ & (n_ - 1)) != 0) throw std::invalid_argument("fft size must be a power of two");
    tw_.resize(n_ / 2);
    for (size_t k = 0; k < n_ / 2; ++k) {
        const double a = -2.0 * kPi * static_cast<double>(k) / static_cast<double>(n_);
        tw_[k] = cf(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
    }
    rev_.resize(n_);
    unsigned bits = 0;
    while ((size_t{1} << bits) < n_) ++bits;
    for (size_t i = 0; i < n_; ++i) {
        uint32_t r = 0;
        for (unsigned b = 0; b < bits; ++b)
            if (i & (size_t{1} << b)) r |= 1u << (bits - 1 - b);
        rev_[i] = r;
    }
}

void Fft::forward(cf* a) const {
    for (size_t i = 0; i < n_; ++i)
        if (i < rev_[i]) std::swap(a[i], a[rev_[i]]);
    for (size_t len = 2, step = n_ / 2; len <= n_; len <<= 1, step >>= 1) {
        const size_t half = len / 2;
        for (size_t i = 0; i < n_; i += len) {
            cf* x = a + i;
            cf* y = a + i + half;
            for (size_t k = 0; k < half; ++k) {
                const cf w = tw_[k * step];
                // Written out so it stays fast whatever the compiler flags
                // (std::complex multiplication checks for NaN otherwise).
                const float vr = y[k].real() * w.real() - y[k].imag() * w.imag();
                const float vi = y[k].real() * w.imag() + y[k].imag() * w.real();
                const cf u = x[k];
                x[k] = cf(u.real() + vr, u.imag() + vi);
                y[k] = cf(u.real() - vr, u.imag() - vi);
            }
        }
    }
}

SpectrumAnalyzer::SpectrumAnalyzer(double sample_rate, size_t fft_size, unsigned ffts_per_line)
    : fs_(sample_rate), fft_(fft_size), per_line_(std::max(1u, ffts_per_line)) {
    window_.resize(fft_size);
    double sum = 0;
    for (size_t i = 0; i < fft_size; ++i) {
        // Blackman-Harris: low sidelobes keep weak SSB signals visible next
        // to the beacons.
        const double x = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(fft_size);
        const double w = 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x) - 0.01168 * std::cos(3 * x);
        window_[i] = static_cast<float>(w);
        sum += w;
    }
    norm_ = static_cast<float>(1.0 / (sum * sum));
    buf_.resize(fft_size);
    acc_.assign(fft_size, 0.0f);
    set_line_rate(line_rate_);
}

void SpectrumAnalyzer::set_line_rate(double lines_per_s) {
    line_rate_ = std::clamp(lines_per_s, 0.5, 50.0);
    const double per_line_samples = fs_ / line_rate_;
    gap_ = std::max(0.0, per_line_samples / per_line_ - static_cast<double>(fft_.size()));
}

void SpectrumAnalyzer::process(const cf* in, size_t n) {
    size_t i = 0;
    while (i < n) {
        if (skip_left_ >= 1.0) {
            const size_t s = std::min(n - i, static_cast<size_t>(skip_left_));
            i += s;
            skip_left_ -= static_cast<double>(s);
            continue;
        }
        const size_t take = std::min(n - i, buf_.size() - fill_);
        std::copy(in + i, in + i + take, buf_.begin() + static_cast<long>(fill_));
        fill_ += take;
        i += take;
        if (fill_ == buf_.size()) {
            finish_fft();
            fill_ = 0;
            skip_left_ += gap_;
        }
    }
}

void SpectrumAnalyzer::finish_fft() {
    const size_t n = buf_.size();
    for (size_t i = 0; i < n; ++i) buf_[i] *= window_[i];
    fft_.forward(buf_.data());
    // fft-shift while accumulating: index 0 = -fs/2.
    const size_t h = n / 2;
    for (size_t i = 0; i < n; ++i) {
        const cf v = buf_[(i + h) & (n - 1)];
        acc_[i] += v.real() * v.real() + v.imag() * v.imag();
    }
    if (++done_ < per_line_) return;
    const float k = norm_ / static_cast<float>(done_);
    ready_.resize(n);
    for (size_t i = 0; i < n; ++i) ready_[i] = acc_[i] * k;
    std::fill(acc_.begin(), acc_.end(), 0.0f);
    done_ = 0;
    has_ready_ = true;
}

bool SpectrumAnalyzer::take_line(std::vector<float>& power) {
    if (!has_ready_) return false;
    power.swap(ready_);
    has_ready_ = false;
    return true;
}

SpectrumView::SpectrumView(double band_center_hz, double band_rate) : center_(band_center_hz), rate_(band_rate) {
    set(SpectrumRequest{});
}

const SpectrumRequest& SpectrumView::set(const SpectrumRequest& r) {
    SpectrumRequest v = r;
    if (!(v.span_hz > 0) || v.span_hz > rate_) v.span_hz = rate_;
    v.span_hz = std::max(v.span_hz, rate_ / 64.0);
    const double lo = center_ - rate_ / 2 + v.span_hz / 2;
    const double hi = center_ + rate_ / 2 - v.span_hz / 2;
    if (!(v.center_hz > 0)) v.center_hz = center_;
    v.center_hz = std::clamp(v.center_hz, lo, hi);
    v.bins = std::clamp(v.bins, 64, 2048);
    v.rate = std::clamp(v.rate, 0.0, 25.0);
    view_ = v;
    return view_;
}

std::string SpectrumView::pack(const std::vector<float>& power, double correction_hz, uint16_t seq) const {
    const size_t n = power.size();
    const int bins = view_.bins;
    const double df = rate_ / static_cast<double>(n);  // input bin width
    const double out_df = view_.span_hz / bins;
    // Raw offset (from the band centre, before correction) of the left edge
    // of output bin 0.
    const double left = view_.center_hz - view_.span_hz / 2 - center_ + correction_hz;

    std::vector<float> db(static_cast<size_t>(bins));
    float lowest = 1e9f;
    for (int k = 0; k < bins; ++k) {
        const double f0 = left + k * out_df, f1 = f0 + out_df;
        // Input bin i is centred on (i - n/2) * df.
        long i0 = static_cast<long>(std::ceil(f0 / df + static_cast<double>(n) / 2));
        long i1 = static_cast<long>(std::ceil(f1 / df + static_cast<double>(n) / 2)) - 1;
        float p = 0.0f;
        if (i1 < i0) {  // zoomed in past the FFT resolution: nearest bin
            i0 = i1 = static_cast<long>(std::lround((f0 + f1) / 2 / df + static_cast<double>(n) / 2));
        }
        // Peak over the covered bins: a CW carrier stays visible when the
        // view is wider than the screen.
        for (long i = std::max(i0, 1L); i <= std::min(i1, static_cast<long>(n) - 1); ++i) p = std::max(p, power[static_cast<size_t>(i)]);
        db[static_cast<size_t>(k)] = p > 0 ? 10.0f * std::log10(p) : -1e9f;
        if (p > 0) lowest = std::min(lowest, db[static_cast<size_t>(k)]);
    }
    if (lowest > 1e8f) lowest = -150.0f;
    const int ref10 = static_cast<int>(std::floor(lowest * 10.0f));  // 0.1 dB
    const float ref = ref10 / 10.0f;
    constexpr int kStep10 = 5;  // 0.5 dB per count: 127 dB of range

    std::string out;
    out.reserve(14 + static_cast<size_t>(bins));
    out.push_back(static_cast<char>(0x02));
    put_u16(out, seq);
    put_u32(out, static_cast<uint32_t>(static_cast<int32_t>(std::lround(view_.center_hz - center_))));
    put_u32(out, static_cast<uint32_t>(std::lround(view_.span_hz)));
    put_u16(out, static_cast<uint16_t>(static_cast<int16_t>(std::clamp(ref10, -32768, 32767))));
    out.push_back(static_cast<char>(kStep10));
    for (float d : db) {
        const long v = d < -1e8f ? 0 : std::lround((d - ref) * 10.0f / kStep10);
        out.push_back(static_cast<char>(std::clamp(v, 0L, 255L)));
    }
    return out;
}

}  // namespace nb
