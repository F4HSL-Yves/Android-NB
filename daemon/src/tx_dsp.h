// Transmit side DSP that does not need WDSP: the upconverter from the 48 kHz
// SSB baseband to the AD9363 DAC rate, and the jitter buffer for the
// microphone packets coming from the phone.
//
// Frequency plan: the AD9363 TX LO stays below the transponder (2399.5 MHz
// by default) so that its leakage and the IQ image fall outside the band;
// the upconverter puts the SSB signal at +offset from the LO with an NCO.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "dsp.h"

namespace nb {

// QO-100 narrowband transponder: uplink = downlink - 8089.5 MHz.
constexpr double kQo100UplinkShiftHz = 8089500000.0;

// CIC interpolator for a complex baseband, integer arithmetic (wraps
// modulo 2^32 in the integrators, which the comb stages undo exactly).
// Its nulls sit on every multiple of the input rate, which is where the
// images of a narrow signal are: with order 4 and a 3 kHz wide signal at
// 48 kS/s, they come out more than 90 dB down.
class CicInterpolator {
public:
    // Input up to +/-max_input (integers); output = input * gain().
    CicInterpolator(unsigned ratio, unsigned order = 4);
    // Appends n * ratio output samples (I, Q interleaved) to out.
    void process(const int32_t* iq, size_t n, std::vector<int32_t>& out);
    void reset();
    unsigned ratio() const { return ratio_; }
    unsigned order() const { return order_; }
    // R^(N-1): the DC gain of an interpolating CIC.
    double gain() const { return gain_; }
    // Split form for a caller that fuses the integrators in its own loop:
    // the comb section at the input rate, then the integrator states.
    void comb(const int32_t* iq, size_t n, int32_t* out);
    uint32_t* integ_i() { return integ_i_.data(); }
    uint32_t* integ_q() { return integ_q_.data(); }

private:
    unsigned ratio_, order_;
    double gain_;
    std::vector<uint32_t> comb_i_, comb_q_, integ_i_, integ_q_;
};

// 48 kHz complex SSB -> int16 IQ at the DAC rate, shifted by offset_hz.
class Upconverter {
public:
    static constexpr double kInRate = 48000.0;
    explicit Upconverter(double out_rate, double full_scale = 0.8);
    // Rejected (std::invalid_argument) outside +/-0.45 out_rate.
    void set_offset_hz(double hz);
    double offset_hz() const { return offset_; }
    unsigned ratio() const { return cic_.ratio(); }
    // in: n samples at 48 kHz, |x| <= 1. out: n * ratio() IQ pairs, int16,
    // 12-bit DAC values MSB aligned (what cf-ad9361-dds-core-lpc expects).
    void process(const cf* in, size_t n, std::vector<int16_t>& out);
    void reset();

private:
    // Integer NCO: 32-bit phase, a table of kNcoBits (cos, sin) pairs with
    // the output scaling folded in. Phase truncation spurs stay near
    // -6 * kNcoBits dBc, well under the AD9363's own.
    static constexpr unsigned kNcoBits = 12;
    double out_rate_;
    double full_scale_;
    double offset_ = 0;
    uint32_t phase_ = 0, inc_ = 0;
    unsigned pre_shift_ = 0;  // integrator output >> pre_shift_ fits 16 bits
    CicInterpolator cic_;
    std::vector<int32_t> in_i32_, wide_;
    std::vector<int16_t> nco_;  // cos, sin interleaved
};

// Microphone packets from the phone, in arrival order with their sequence
// number. pop() gives them back in sequence order once `prefill` packets
// are queued, reports a missing packet (for loss concealment) and drops the
// oldest when the backlog grows past `max_queue`.
class PacketJitter {
public:
    enum class Pop { Packet, Missing, Empty };
    explicit PacketJitter(size_t prefill = 4, size_t max_queue = 15) : prefill_(prefill), max_(max_queue) {}
    void push(uint16_t seq, std::string packet);
    Pop pop(std::string& packet);
    void reset();
    size_t size() const;
    uint64_t underruns() const { return underruns_; }
    uint64_t dropped() const { return dropped_; }

private:
    mutable std::mutex mu_;
    size_t prefill_, max_;
    std::deque<std::pair<uint16_t, std::string>> q_;
    bool started_ = false;
    bool have_next_ = false;
    uint16_t next_ = 0;
    uint64_t underruns_ = 0, dropped_ = 0;
};

// Automatic TX frequency correction (full duplex): with the tune carrier
// on, nbd hears its own carrier back from the transponder in the
// beacon-corrected receive baseband (centred on the dial, 48 kS/s). It
// should sit at +expected_hz (the 1 kHz tone); the difference is the
// uplink error, mostly the board's crystal (the beacon only corrects the
// receive side, LNB drift included).
class TxCalibrator {
public:
    enum class Step { Idle, Running, Done, Failed };
    explicit TxCalibrator(double rate, size_t fft_size = 65536, double settle_s = 1.2);
    void start(double expected_hz, double cal_hz);
    void cancel() { step_ = Step::Idle; }
    bool active() const { return step_ == Step::Running; }
    // Feeds the receive baseband of one block lasting block_s. carrier_on:
    // the transmitter is on with the tune carrier; locked: beacon lock.
    // Returns Done (result()/message()) or Failed (message()) once.
    Step feed(const cf* x, size_t n, double block_s, bool carrier_on, bool locked);
    double new_cal_hz() const { return new_cal_; }
    double measured_hz() const { return measured_; }
    double snr_db() const { return snr_; }
    const std::string& message() const { return msg_; }

    static constexpr double kMaxStepHz = 6000.0;   // search +/- around the expected tone
    static constexpr double kMaxCalHz = 20000.0;   // about 8 ppm at 2.4 GHz

private:
    Step finish(Step s, std::string msg) {
        step_ = Step::Idle;
        msg_ = std::move(msg);
        return s;
    }
    double rate_, settle_s_;
    size_t n_;
    Step step_ = Step::Idle;
    double expected_ = 1000, cal_ = 0, waited_ = 0;
    double new_cal_ = 0, measured_ = 0, snr_ = 0;
    std::string msg_;
    std::vector<cf> buf_;
};

}  // namespace nb
