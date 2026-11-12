// Live IQ source on a Pluto / FishBall running Tezuka + Maia SDR.
//
// Sets the AD9363 (LO, sample rate, gain), designs the Maia DDC through
// maia-httpd, routes the DDC output to the RX DMA (register 0x800000BC of
// cf-ad9361-lpc, the F5OEO addition used by Android-DATV) and streams the
// decimated IQ through iiod. Everything goes over TCP, so the same code runs
// on the board (127.0.0.1) and on a PC or phone on the same network.
//
// Frequency plan: the AD9363 LO sits lo_offset_hz above the wanted centre so
// that its DC spur and IQ image stay out of the transponder; the DDC mixer
// brings the centre back to 0 Hz.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dsp.h"
#include "iiod_client.h"

namespace nb {

struct PlutoConfig {
    std::string host = "127.0.0.1";
    int iiod_port = 30431;
    int http_port = 80;
    double center_hz = 10489750000.0;  // downlink frequency at the IQ centre
    double lnb_lo_hz = 9750000000.0;   // nominal LNB local oscillator
    double lo_offset_hz = 500000.0;    // AD9363 LO above the centre
    long input_rate = 3072000;         // AD9363 sample rate
    int decimation = 4;                // Maia DDC: 3.072 MS/s / 4 = 768 kS/s
    long rf_bandwidth = 2000000;
    std::string gain_mode = "slow_attack";  // or manual, fast_attack, hybrid
    double gain_db = 40.0;                  // used when gain_mode is manual
    size_t buffer_samples = 32768;          // multiple of 4096 (Tezuka DMA)
    bool spectrometer_on_ddc = true;        // Maia waterfall shows the NB band
};

// Values derived from the config, exposed for tests and logs.
struct PlutoPlan {
    int64_t rx_lo_hz = 0;
    double ddc_frequency_hz = 0.0;
    double output_rate = 0.0;
};
PlutoPlan make_plan(const PlutoConfig& cfg);
std::string ddc_design_json(const PlutoConfig& cfg);

class PlutoSource {
public:
    explicit PlutoSource(PlutoConfig cfg);
    ~PlutoSource();
    PlutoSource(const PlutoSource&) = delete;
    PlutoSource& operator=(const PlutoSource&) = delete;

    // Configures the board and opens the RX buffer. Throws on failure.
    void start();
    // Next block of buffer_samples IQ samples, scaled to +/-1.
    void read(std::vector<cf>& out);
    // Closes the buffer and gives the DMA back to the AD9363. Safe to repeat.
    void stop();

    // Receiver gain while streaming: "manual" with db, or an AGC mode
    // (slow_attack...). Uses its own iiod connection. Throws on refusal.
    void set_gain(const std::string& mode, double db);
    const std::string& gain_mode() const { return cfg_.gain_mode; }
    double gain_db() const { return cfg_.gain_db; }

    double output_rate() const { return output_rate_; }
    const std::string& summary() const { return summary_; }

private:
    void set_route(bool enabled);

    PlutoConfig cfg_;
    IiodClient iiod_;
    IioDeviceInfo phy_;
    IioDeviceInfo adc_;
    std::string mask_;
    bool open_ = false;
    bool routed_ = false;
    double output_rate_ = 0.0;
    std::string summary_;
    std::vector<uint8_t> raw_;
};

}  // namespace nb
