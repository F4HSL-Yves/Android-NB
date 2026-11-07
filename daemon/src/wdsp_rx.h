// One WDSP receive channel (one VFO) fed with the wide IQ stream.
#pragma once

#include <cstddef>
#include <vector>

#include "dsp.h"

namespace nb {

class WdspRx {
public:
    // in_size: IQ samples per call to process(); in_size * 48000 / in_rate
    // must be a whole number.
    // dsp_size: WDSP processing block at 48 kHz (filter resolution vs delay).
    WdspRx(int channel, int in_rate, int in_size, int dsp_size = 4096);
    ~WdspRx();
    WdspRx(const WdspRx&) = delete;
    WdspRx& operator=(const WdspRx&) = delete;

    // Position of the wanted station relative to the stream centre, in Hz,
    // beacon correction included.
    void set_offset_hz(double hz);
    // Upper sideband audio between low_hz and high_hz.
    void set_usb(double low_hz, double high_hz);
    // 0 off, 1 long, 2 slow, 3 medium (default), 4 fast.
    void set_agc(int mode);

    // in: exactly in_size() samples. Appends 48 kHz mono audio to `audio`.
    void process(const cf* in, std::vector<float>& audio);

    int in_size() const { return in_size_; }
    int out_size() const { return out_size_; }
    // Average S-meter reading, dB.
    double signal_db() const;

    static constexpr int kAudioRate = 48000;

private:
    int channel_;
    int in_size_;
    int out_size_;
    std::vector<double> in_;
    std::vector<double> out_;
};

// Precompute (or load) FFTW plans for WDSP. Slow the first time on the
// Cortex-A9; the result is cached in `directory`.
void wdsp_wisdom(const char* directory);

}  // namespace nb
