// Small DSP building blocks shared by the beacon lock and the daemon.
// No external dependency, so the core can be unit tested on any host.
#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace nb {

using cf = std::complex<float>;

// Windowed-sinc (Blackman) low-pass FIR with unity gain at DC.
// cutoff is a fraction of the sample rate (0 < cutoff < 0.5).
std::vector<float> design_lowpass(size_t taps, double cutoff);

// Number of Blackman taps needed for a transition band of `transition`
// (fraction of the sample rate), rounded up to an odd count.
size_t lowpass_taps(double transition);

// Numerically controlled oscillator used as a frequency shifter.
class Nco {
public:
    explicit Nco(double sample_rate);
    void set_frequency(double hz);
    double frequency() const { return freq_; }
    // out[i] = in[i] * exp(-j 2 pi f t): a signal sitting at +f lands on 0 Hz.
    // in and out may be the same buffer.
    void mix_down(const cf* in, cf* out, size_t n);
    void reset();

private:
    double fs_;
    double freq_ = 0.0;
    std::complex<double> rot_{1.0, 0.0};
    std::complex<double> step_{1.0, 0.0};
    unsigned since_norm_ = 0;
};

// FIR low-pass followed by decimation. Outputs are appended to `out`.
class FirDecimator {
public:
    FirDecimator() = default;
    FirDecimator(std::vector<float> taps, unsigned decimation);
    void process(const cf* in, size_t n, std::vector<cf>& out);
    void reset();
    unsigned decimation() const { return decim_; }

private:
    std::vector<float> taps_;
    unsigned decim_ = 1;
    std::vector<cf> hist_;  // doubled delay line: no wrap test in the MAC loop
    size_t pos_ = 0;
    unsigned phase_ = 0;
};

// Low-pass decimator whose output is free of aliasing within +/- passband_hz.
FirDecimator make_decimator(double in_rate, unsigned decim, double passband_hz);

// Shifts a station to 0 Hz and decimates the wide IQ stream around it, so
// that the demodulator works at a low rate (768 kS/s -> 48 kS/s).
class Downconverter {
public:
    Downconverter(double in_rate, unsigned decim, double passband_hz);
    void set_frequency(double hz) { nco_.set_frequency(hz); }
    // Appends n / decim samples (n a multiple of decim keeps blocks exact).
    void process(const cf* in, size_t n, std::vector<cf>& out);
    double out_rate() const { return out_rate_; }

private:
    Nco nco_;
    FirDecimator fir_;
    std::vector<cf> mixed_;
    double out_rate_;
};

// In-place iterative radix-2 FFT. a.size() must be a power of two.
void fft_inplace(std::vector<std::complex<double>>& a);

}  // namespace nb
