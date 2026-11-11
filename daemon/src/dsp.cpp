#include "dsp.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace nb {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

std::vector<float> design_lowpass(size_t taps, double cutoff) {
    if (taps < 3 || cutoff <= 0.0 || cutoff >= 0.5) {
        throw std::invalid_argument("design_lowpass: bad parameters");
    }
    std::vector<double> h(taps);
    const double m = (taps - 1) / 2.0;
    double sum = 0.0;
    for (size_t k = 0; k < taps; ++k) {
        const double x = k - m;
        const double sinc = (x == 0.0) ? 2.0 * cutoff : std::sin(2.0 * kPi * cutoff * x) / (kPi * x);
        const double w = 0.42 - 0.5 * std::cos(2.0 * kPi * k / (taps - 1)) +
                         0.08 * std::cos(4.0 * kPi * k / (taps - 1));
        h[k] = sinc * w;
        sum += h[k];
    }
    std::vector<float> out(taps);
    for (size_t k = 0; k < taps; ++k) out[k] = static_cast<float>(h[k] / sum);
    return out;
}

size_t lowpass_taps(double transition) {
    // Blackman: transition width ~ 5.5 / N.
    size_t n = static_cast<size_t>(std::ceil(5.5 / transition));
    if (n < 15) n = 15;
    return n | 1u;
}

Nco::Nco(double sample_rate) : fs_(sample_rate) {}

void Nco::set_frequency(double hz) {
    freq_ = hz;
    const double w = -2.0 * kPi * hz / fs_;
    step_ = {std::cos(w), std::sin(w)};
}

void Nco::reset() {
    rot_ = {1.0, 0.0};
    since_norm_ = 0;
}

void Nco::mix_down(const cf* in, cf* out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const cf r(static_cast<float>(rot_.real()), static_cast<float>(rot_.imag()));
        out[i] = in[i] * r;
        rot_ *= step_;
        if (++since_norm_ == 1024) {
            rot_ /= std::abs(rot_);
            since_norm_ = 0;
        }
    }
}

FirDecimator::FirDecimator(std::vector<float> taps, unsigned decimation)
    : taps_(std::move(taps)), decim_(decimation ? decimation : 1), hist_(2 * taps_.size()) {}

void FirDecimator::reset() {
    std::fill(hist_.begin(), hist_.end(), cf(0.0f, 0.0f));
    pos_ = 0;
    phase_ = 0;
}

void FirDecimator::process(const cf* in, size_t n, std::vector<cf>& out) {
    const size_t len = taps_.size();
    for (size_t i = 0; i < n; ++i) {
        pos_ = (pos_ == 0 ? len : pos_) - 1;  // newest sample at pos_
        hist_[pos_] = in[i];
        hist_[pos_ + len] = in[i];
        if (++phase_ == decim_) {
            phase_ = 0;
            const cf* h = &hist_[pos_];
            float re = 0.0f, im = 0.0f;
            for (size_t k = 0; k < len; ++k) {
                re += h[k].real() * taps_[k];
                im += h[k].imag() * taps_[k];
            }
            out.emplace_back(re, im);
        }
    }
}

FirDecimator make_decimator(double in_rate, unsigned decim, double passband_hz) {
    const double out_rate = in_rate / decim;
    const double stop = out_rate - passband_hz;
    const size_t taps = lowpass_taps((stop - passband_hz) / in_rate);
    return FirDecimator(design_lowpass(taps, 0.5 * (passband_hz + stop) / in_rate), decim);
}

Downconverter::Downconverter(double in_rate, unsigned decim, double passband_hz)
    : nco_(in_rate), fir_(make_decimator(in_rate, decim, passband_hz)), out_rate_(in_rate / decim) {}

void Downconverter::process(const cf* in, size_t n, std::vector<cf>& out) {
    mixed_.resize(n);
    nco_.mix_down(in, mixed_.data(), n);
    fir_.process(mixed_.data(), n, out);
}

void fft_inplace(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    if (n == 0 || (n & (n - 1)) != 0) throw std::invalid_argument("fft size must be a power of two");
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

}  // namespace nb
