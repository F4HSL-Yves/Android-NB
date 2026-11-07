#include "wdsp_rx.h"

#include <stdexcept>
#include <string>

// WDSP's own wdsp.h opens an extern "C" block it never closes, so the few
// entry points used here are declared directly (signatures from wdsp.h).
extern "C" {
void OpenChannel(int channel, int in_size, int dsp_size, int input_samplerate, int dsp_rate,
                 int output_samplerate, int type, int state, double tdelayup, double tslewup,
                 double tdelaydown, double tslewdown, int bfo);
void CloseChannel(int channel);
void fexchange0(int channel, double* in, double* out, int* error);
void SetRXAMode(int channel, int mode);
void SetRXABandpassFreqs(int channel, double f_low, double f_high);
void SetRXAAGCMode(int channel, int mode);
void SetRXAShiftRun(int channel, int run);
void SetRXAShiftFreq(int channel, double fshift);
void SetRXAPanelGain1(int channel, double gain);
double GetRXAMeter(int channel, int mt);
void WDSPwisdom(char* directory);
}

namespace nb {

namespace {
// Values of WDSP's rxaMode and AGC mode enums (RXA.h, wcpAGC.c).
constexpr int kModeUsb = 1;
constexpr int kAgcMedium = 3;
constexpr int kMeterSignalAverage = 1;  // RXA_S_AV
}  // namespace

WdspRx::WdspRx(int channel, int in_rate, int in_size, int dsp_size)
    : channel_(channel), in_size_(in_size) {
    if (in_size <= 0 || (static_cast<long>(in_size) * kAudioRate) % in_rate != 0) {
        throw std::invalid_argument("WdspRx: in_size * 48000 must be a multiple of in_rate");
    }
    out_size_ = static_cast<int>(static_cast<long>(in_size) * kAudioRate / in_rate);
    in_.assign(2 * static_cast<size_t>(in_size_), 0.0);
    out_.assign(2 * static_cast<size_t>(out_size_), 0.0);

    // type 0 = receive, state 1 = running, bfo 1 = fexchange0 waits for
    // output, which keeps the offline tool strictly sequential.
    OpenChannel(channel_, in_size_, dsp_size, in_rate, kAudioRate, kAudioRate, 0, 1, 0.0, 0.010, 0.0, 0.010, 1);
    SetRXAMode(channel_, kModeUsb);
    SetRXABandpassFreqs(channel_, 200.0, 2800.0);
    SetRXAAGCMode(channel_, kAgcMedium);
    SetRXAShiftRun(channel_, 1);
    SetRXAShiftFreq(channel_, 0.0);
    // The AGC drives peaks to about 1.0 and beyond; leave headroom so the
    // 16-bit and Opus outputs do not clip.
    SetRXAPanelGain1(channel_, 0.5);
}

WdspRx::~WdspRx() { CloseChannel(channel_); }

void WdspRx::set_offset_hz(double hz) {
    // WDSP's shifter multiplies by exp(+j...): move the station down to 0 Hz.
    SetRXAShiftFreq(channel_, -hz);
}

void WdspRx::set_agc(int mode) { SetRXAAGCMode(channel_, mode); }

void WdspRx::set_usb(double low_hz, double high_hz) {
    SetRXAMode(channel_, kModeUsb);
    SetRXABandpassFreqs(channel_, low_hz, high_hz);
}

void WdspRx::process(const cf* in, std::vector<float>& audio) {
    for (int i = 0; i < in_size_; ++i) {
        in_[2 * i] = in[i].real();
        in_[2 * i + 1] = in[i].imag();
    }
    int error = 0;
    fexchange0(channel_, in_.data(), out_.data(), &error);
    for (int i = 0; i < out_size_; ++i) audio.push_back(static_cast<float>(out_[2 * i]));
}

double WdspRx::signal_db() const { return GetRXAMeter(channel_, kMeterSignalAverage); }

void wdsp_wisdom(const char* directory) {
    std::string dir(directory);
    if (!dir.empty() && dir.back() != '/') dir.push_back('/');
    WDSPwisdom(dir.data());
}

}  // namespace nb
