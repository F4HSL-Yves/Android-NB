// Continuous lock on the QO-100 narrowband CW beacon.
//
// The beacon (10489.500 MHz) is searched for in a wide window, then followed
// by a frequency-locked loop. The result, correction_hz(), is how far the
// whole IQ stream is off (LNB drift + SDR reference error): shifting the
// stream down by that amount puts every station back on its true frequency.
//
// Acquisition: shift the expected beacon position to 0 Hz, decimate to a
// rate a bit wider than the search window, take a ~1 s FFT and keep the
// strongest bin if it clears the SNR threshold.
//
// Tracking: shift by the current estimate, decimate to 1 kS/s through two
// FIR stages, and measure the residual frequency every 40 ms from the
// phase advance between consecutive samples. Blocks where the signal is not
// a coherent carrier (CW key-up, noise, a passing SSB station) are skipped,
// so the loop simply coasts on its drift-rate estimate between CW elements.
// A second-order loop follows a steadily drifting (warming) LNB with no
// standing error.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "dsp.h"

namespace nb {

struct BeaconLockConfig {
    double sample_rate = 768000.0;
    // Where the beacon should sit relative to the stream centre, in Hz,
    // if the LNB and the SDR were perfect.
    double beacon_offset_hz = -250000.0;
    // Half width of the acquisition search, in Hz.
    double search_half_width_hz = 25000.0;
    // Minimum peak-to-median ratio to accept an acquisition, in dB.
    double acquisition_snr_db = 12.0;
    // Natural frequency of the tracking loop, in Hz. Lower = smoother,
    // higher = follows faster drift.
    double loop_natural_hz = 0.2;
    // Seconds without a coherent block before reporting holdover,
    // and before going back to a full search.
    double holdover_after_s = 5.0;
    double reacquire_after_s = 60.0;
};

enum class LockState { Searching, Locked, Holdover };

const char* to_string(LockState s);

struct BeaconLockStatus {
    LockState state = LockState::Searching;
    double correction_hz = 0.0;   // measured beacon position minus expected
    double drift_hz_per_s = 0.0;  // current drift rate estimate
    double acquisition_snr_db = 0.0;
    double seconds_since_carrier = 0.0;
};

class BeaconLock {
public:
    explicit BeaconLock(const BeaconLockConfig& cfg);

    // Feed IQ samples at cfg.sample_rate. Safe to call with any block size.
    void process(const cf* iq, size_t n);

    BeaconLockStatus status() const;
    double correction_hz() const { return freq_ - cfg_.beacon_offset_hz; }
    LockState state() const { return state_; }

private:
    void acquire_block(const cf* iq, size_t n);
    void run_acquisition();
    void track_block(const cf* iq, size_t n);
    void loop_update(double error_hz);
    void coast(double dt);
    void enter_tracking(double beacon_freq_hz);

    BeaconLockConfig cfg_;
    LockState state_ = LockState::Searching;
    uint64_t samples_ = 0;  // total samples seen, our clock

    // Acquisition chain.
    Nco acq_nco_;
    FirDecimator acq_fir_;
    double acq_rate_ = 0.0;
    size_t acq_fft_size_ = 0;
    std::vector<cf> acq_buf_;
    double acq_snr_db_ = 0.0;

    // Tracking chain.
    Nco trk_nco_;
    FirDecimator trk_fir1_;
    FirDecimator trk_fir2_;
    double trk_rate_ = 0.0;  // rate after both stages (~1 kS/s)
    std::vector<cf> stage1_;
    std::vector<cf> stage2_;
    cf prev_{0.0f, 0.0f};
    std::complex<double> prod_{0.0, 0.0};
    double prod_mag_ = 0.0;
    unsigned block_count_ = 0;
    unsigned block_len_ = 40;

    // Loop state.
    double freq_ = 0.0;  // current beacon position estimate, Hz from centre
    double rate_ = 0.0;  // Hz per second
    double k1_ = 0.0, k2_ = 0.0;
    uint64_t last_carrier_sample_ = 0;

    std::vector<cf> scratch_;
};

}  // namespace nb
