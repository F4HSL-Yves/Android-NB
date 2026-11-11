// Waterfall for the phone app.
//
// A spectrum "line" is the power of the whole IQ band in fft-shifted order
// (index 0 = centre - rate/2), linear units, one line per waterfall row.
// It comes either from Maia's FPGA spectrometer (no CPU on the board, see
// maia_waterfall.h) or from SpectrumAnalyzer below (any source).
//
// SpectrumView then cuts the part the app looks at, in corrected downlink
// frequencies (the LNB drift measured on the beacon is taken out), reduces
// it to the app's pixel count and packs it as 8-bit dB for the WebSocket.
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "dsp.h"

namespace nb {

// Float radix-2 FFT with precomputed twiddles, in place. size: power of two.
class Fft {
public:
    explicit Fft(size_t size);
    void forward(cf* a) const;
    size_t size() const { return n_; }

private:
    size_t n_;
    std::vector<cf> tw_;
    std::vector<uint32_t> rev_;
};

// Averaged periodogram of the IQ stream, only a few windowed FFTs per line
// so the cost stays small on the Zynq (nbbench measures it; Maia's FPGA
// spectrometer is preferred when live). A full scale tone reads 1.0 (0 dBFS).
class SpectrumAnalyzer {
public:
    SpectrumAnalyzer(double sample_rate, size_t fft_size = 4096, unsigned ffts_per_line = 2);
    void set_line_rate(double lines_per_s);
    double line_rate() const { return line_rate_; }
    void process(const cf* in, size_t n);
    // The newest finished line, if any since the last call.
    bool take_line(std::vector<float>& power);
    size_t bins() const { return fft_.size(); }

private:
    void finish_fft();

    double fs_;
    Fft fft_;
    unsigned per_line_;
    double line_rate_ = 8.0;
    std::vector<float> window_;
    float norm_;
    std::vector<cf> buf_;
    size_t fill_ = 0;
    std::vector<float> acc_;
    unsigned done_ = 0;     // FFTs in acc_
    double gap_ = 0;        // samples to skip before the next FFT
    double skip_left_ = 0;
    std::vector<float> ready_;
    bool has_ready_ = false;
};

struct SpectrumRequest {
    double center_hz = 0;  // view centre, downlink frequency; 0: centre of the band
    double span_hz = 0;    // 0: the whole band
    int bins = 768;
    double rate = 8.0;     // lines per second, 0 turns the waterfall off
};

// Packs a line for the app (binary type 0x02, see protocol.h).
class SpectrumView {
public:
    SpectrumView(double band_center_hz, double band_rate);
    // Clamps the request to what the band allows; returns the view in force.
    const SpectrumRequest& set(const SpectrumRequest& r);
    const SpectrumRequest& view() const { return view_; }
    // power: one line over the band (index 0 = centre - rate/2).
    // correction_hz: measured LNB error, raw = true + correction.
    std::string pack(const std::vector<float>& power, double correction_hz, uint16_t seq) const;

private:
    double center_, rate_;
    SpectrumRequest view_;
};

}  // namespace nb
