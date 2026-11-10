// WDSP transmit channel: microphone (48 kHz mono) -> USB complex baseband
// at 48 kHz, with WDSP's leveler, optional compressor and ALC, so the level
// sent to the DAC stays under control whatever the phone's microphone does.
// The "tune" carrier comes from WDSP's output generator.
#pragma once

#include <vector>

#include "dsp.h"

namespace nb {

class WdspTx {
public:
    static constexpr int kRate = 48000;

    // block: microphone samples per process() call.
    explicit WdspTx(int channel, int block = 512, int dsp_size = 1024);
    ~WdspTx();
    WdspTx(const WdspTx&) = delete;
    WdspTx& operator=(const WdspTx&) = delete;

    void set_passband(double low_hz, double high_hz);  // audio, USB
    void set_mic_gain_db(double db);                   // before the leveler
    void set_compressor(bool on, double gain_db = 6.0);
    // Carrier at dial + hz instead of the microphone (magnitude 0..1).
    // Full scale by default: the carrier stands for the voice peaks (the ALC
    // holds them at 0 dB), so the PA is set up for the PEP.
    void set_tune(bool on, double hz = 1000.0, double magnitude = 0.99);
    bool tune() const { return tune_; }

    // mic: block() samples. out: block() complex samples, USB above 0 Hz.
    void process(const float* mic, std::vector<cf>& out);
    // Clears filters and generators (start of a transmission).
    void flush();

    int block() const { return block_; }
    double mic_peak_db() const;
    double alc_gain_db() const;
    double out_peak_db() const;

private:
    int channel_;
    int block_;
    bool tune_ = false;
    std::vector<double> in_, out_;
};

}  // namespace nb
