#include "wdsp_tx.h"

#include <cmath>
#include <stdexcept>

// WDSP's own wdsp.h opens an extern "C" block it never closes, so the entry
// points used here are declared directly (signatures from the WDSP sources).
extern "C" {
void OpenChannel(int channel, int in_size, int dsp_size, int input_samplerate, int dsp_rate,
                 int output_samplerate, int type, int state, double tdelayup, double tslewup,
                 double tdelaydown, double tslewdown, int bfo);
void CloseChannel(int channel);
int SetChannelState(int channel, int state, int dmode);
void fexchange0(int channel, double* in, double* out, int* error);
void SetTXAMode(int channel, int mode);
void SetTXABandpassFreqs(int channel, double f_low, double f_high);
void SetTXAPanelGain1(int channel, double gain);
void SetTXALevelerSt(int channel, int state);
void SetTXALevelerTop(int channel, double maxgain);
void SetTXAALCSt(int channel, int state);
void SetTXACompressorRun(int channel, int run);
void SetTXACompressorGain(int channel, double gain);
void SetTXAPostGenRun(int channel, int run);
void SetTXAPostGenMode(int channel, int mode);
void SetTXAPostGenToneMag(int channel, double mag);
void SetTXAPostGenToneFreq(int channel, double freq);
double GetTXAMeter(int channel, int mt);
void SetTXAEQRun(int channel, int run);
void SetTXAEQProfile(int channel, int nfreqs, double* F, double* G);
}

namespace nb {

namespace {
// WDSP enums (TXA.h).
constexpr int kTxaUsb = 1;
constexpr int kMicPeak = 0;    // TXA_MIC_PK
constexpr int kAlcGain = 14;   // TXA_ALC_GAIN
constexpr int kOutPeak = 15;   // TXA_OUT_PK
// WDSP's TXA works in the mirrored convention (Hermes / Thetis): a USB
// passband given as +200..+2800 Hz comes out as exp(-j w t), below 0 Hz.
// nbd wants USB above 0 Hz, so the output is conjugated. test_tx checks it.
constexpr bool kConjugate = true;
// The ALC holds the peaks at 0 dB; the output sits 3 dB below that so the
// DAC and the PA keep some headroom (the tune carrier included, so a PA set
// up with it is still set for the voice peaks).
constexpr float kOutScale = 0.708f;
}  // namespace

WdspTx::WdspTx(int channel, int block, int dsp_size) : channel_(channel), block_(block) {
    if (block <= 0 || dsp_size < block) throw std::invalid_argument("WdspTx: bad block size");
    in_.assign(2 * static_cast<size_t>(block_), 0.0);
    out_.assign(2 * static_cast<size_t>(block_), 0.0);
    // type 1 = transmit, state 1 = running, bfo 1: fexchange0 waits for its
    // output, so the caller (the DAC pace) drives the channel.
    OpenChannel(channel_, block_, dsp_size, kRate, kRate, kRate, 1, 1, 0.0, 0.010, 0.0, 0.010, 1);
    SetTXAMode(channel_, kTxaUsb);
    set_passband(200.0, 2800.0);
    SetTXALevelerSt(channel_, 1);
    SetTXALevelerTop(channel_, 15.0);
    SetTXAALCSt(channel_, 1);
    SetTXACompressorRun(channel_, 0);
    SetTXAPanelGain1(channel_, 1.0);
    SetTXAPostGenMode(channel_, 0);  // tone
    SetTXAPostGenRun(channel_, 0);
}

WdspTx::~WdspTx() { CloseChannel(channel_); }

void WdspTx::set_passband(double low_hz, double high_hz) {
    // Positive in WDSP's own convention, whatever the conjugation after it.
    SetTXABandpassFreqs(channel_, low_hz, high_hz);
}

void WdspTx::set_mic_gain_db(double db) { SetTXAPanelGain1(channel_, std::pow(10.0, db / 20.0)); }

void WdspTx::set_compressor(bool on, double gain_db) {
    SetTXACompressorGain(channel_, gain_db);
    SetTXACompressorRun(channel_, on ? 1 : 0);
}

void WdspTx::set_tune(bool on, double hz, double magnitude) {
    tune_ = on;
    // WDSP's tone generator makes exp(-j w t), i.e. +hz in its convention.
    SetTXAPostGenToneFreq(channel_, kConjugate ? hz : -hz);
    SetTXAPostGenToneMag(channel_, magnitude);
    SetTXAPostGenRun(channel_, on ? 1 : 0);
}

void WdspTx::set_eq(double bass_db, double treble_db) {
    if (bass_db == 0.0 && treble_db == 0.0) {
        SetTXAEQRun(channel_, 0);
        return;
    }
    // F[0] / G[0] are WDSP's preamp slot; the curve goes through the points.
    double F[] = {0.0, 150.0, 400.0, 1000.0, 2000.0, 2900.0};
    double G[] = {0.0, bass_db, bass_db * 0.5, 0.0, treble_db * 0.6, treble_db};
    SetTXAEQRun(channel_, 0);
    SetTXAEQProfile(channel_, 5, F, G);
    SetTXAEQRun(channel_, 1);
}

void WdspTx::flush() {
    SetChannelState(channel_, 0, 1);
    SetChannelState(channel_, 1, 0);
}

void WdspTx::process(const float* mic, std::vector<cf>& out) {
    for (int i = 0; i < block_; ++i) {
        const double v = tune_ ? 0.0 : static_cast<double>(mic[i]);
        in_[2 * i] = v;
        in_[2 * i + 1] = 0.0;
    }
    int error = 0;
    fexchange0(channel_, in_.data(), out_.data(), &error);
    out.resize(static_cast<size_t>(block_));
    for (int i = 0; i < block_; ++i) {
        const float re = static_cast<float>(out_[2 * i]) * kOutScale;
        const float im = static_cast<float>(out_[2 * i + 1]) * kOutScale;
        out[static_cast<size_t>(i)] = kConjugate ? cf(re, -im) : cf(re, im);
    }
}

double WdspTx::mic_peak_db() const { return GetTXAMeter(channel_, kMicPeak); }
double WdspTx::alc_gain_db() const { return GetTXAMeter(channel_, kAlcGain); }
double WdspTx::out_peak_db() const { return GetTXAMeter(channel_, kOutPeak); }

}  // namespace nb
