// SSB transmitter of nbd: microphone packets from the phone -> Opus decode
// -> WDSP TX -> upconverter -> TX DMA, in its own thread paced by the DAC.
//
// One client at a time owns the transmitter (the one that asked for it).
// The RF itself is switched by the app over MQTT, like Android-DATV does
// (Arduino PTT, then tx/mute 0). nbd mutes the AD9363 whenever it stops,
// and stops on its own, calling `on_emergency` (Arduino PTT off), when
// the owner disconnects, when its microphone stream stops for more than
// mic_timeout_s, or after max_tx_s.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "pluto_tx.h"
#include "tx_dsp.h"

namespace nb {

class WdspTx;

struct TransmitterConfig {
    double uplink_shift_hz = kQo100UplinkShiftHz;
    double min_uplink_hz = 2400000000.0;  // NB transponder uplink
    double max_uplink_hz = 2400500000.0;
    double max_tx_s = 600.0;
    double mic_timeout_s = 1.0;
    int block = 512;          // 48 kHz samples per DSP block
    int wdsp_channel = 1;     // channel 0 is the receiver
    int dsp_size = 1024;
};

struct TxStatus {
    std::string state = "off";  // off, starting, on, error
    std::string message;        // why it stopped or failed
    int owner = -1;
    double uplink_hz = 0, lo_hz = 0, cal_hz = 0;
    bool tune = false;
    double mic_db = -200, alc_db = 0, out_db = -200;
    uint64_t underruns = 0, dropped = 0;
    double on_s = 0;
    double dac_sps = 0;  // IQ samples actually written per second (should be the DAC rate)
};
std::string tx_status_json(const TxStatus& s);

class Transmitter {
public:
    using SinkFactory = std::function<std::unique_ptr<TxSink>()>;
    Transmitter(TransmitterConfig cfg, SinkFactory make_sink);
    ~Transmitter();
    Transmitter(const Transmitter&) = delete;
    Transmitter& operator=(const Transmitter&) = delete;

    // All of these may be called from any thread.
    // on: returns an error message (empty when accepted).
    std::string request(int client, bool on);
    void set_dial(double downlink_hz);
    void set_cal(double hz);
    void set_tune(bool on, double audio_hz);
    void set_mic_gain_db(double db);
    void set_passband(double low_hz, double high_hz);
    void mic_packet(int client, uint16_t seq, std::string opus);
    void client_closed(int client);

    TxStatus status() const;
    // Called from the transmitter thread on each state change, and every
    // 200 ms while on (meters).
    std::function<void(const TxStatus&)> on_status;
    // Called when nbd stops a transmission by itself (Arduino PTT off).
    std::function<void(const std::string& reason)> on_emergency;

private:
    void run();
    void transmit();
    void finish(const std::string& state, const std::string& message, bool emergency);
    void notify();

    TransmitterConfig cfg_;
    SinkFactory make_sink_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::thread thread_;
    bool stop_ = false;
    bool want_on_ = false;
    int owner_ = -1;
    TxStatus st_;
    double dial_hz_ = 10489750000.0, cal_hz_ = 0, tune_hz_ = 1000;
    bool tune_ = false, tune_dirty_ = true;
    double mic_gain_db_ = 0;
    bool mic_gain_dirty_ = true;
    double pass_low_ = 200, pass_high_ = 2800;
    bool pass_dirty_ = true;
    std::chrono::steady_clock::time_point last_mic_;
    std::string stop_reason_;
    bool stop_emergency_ = false;
    PacketJitter jitter_;
};

}  // namespace nb
