// Transmit output of nbd: the DAC side of the AD9363.
//
// PlutoTx streams int16 IQ to cf-ad9361-dds-core-lpc through iiod (its own
// TCP connection, the receive one stays busy reading). The AD9363 shares its
// sample rate between RX and TX, so the DAC runs at the 3.072 MS/s nbd set
// for reception; PlutoTx only checks it. It sets the TX LO (below the
// transponder) and can cut the RF at once (TX LO power down), which is
// what Tezuka's tx/mute does.
//
// pluto_stream must leave the DMA free: its tx/stream/mode has to be "pass"
// (the app sets it over MQTT before the first transmission).
#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <string>

#include "iiod_client.h"

namespace nb {

class TxSink {
public:
    virtual ~TxSink() = default;
    // Configures the TX side and opens the buffer, RF still muted. Throws.
    virtual void open() = 0;
    virtual double rate() const = 0;   // DAC sample rate
    virtual double lo_hz() const = 0;  // TX LO in force
    // samples IQ pairs; blocks at the DAC pace on a real board.
    virtual void write(const int16_t* iq, size_t samples) = 0;
    virtual void close() = 0;
    // Cuts the RF now (TX LO power down). Must not throw.
    virtual void mute() noexcept = 0;
};

struct PlutoTxConfig {
    std::string host = "127.0.0.1";
    int iiod_port = 30431;
    double lo_hz = 2399500000.0;   // below the NB uplink (2400.000 - 2400.500 MHz)
    double expected_rate = 3072000.0;
    size_t buffer_samples = 32768;  // ~10.7 ms per DMA block
    int kernel_buffers = 4;
};

class PlutoTx : public TxSink {
public:
    explicit PlutoTx(PlutoTxConfig cfg) : cfg_(std::move(cfg)) {}
    ~PlutoTx() override;
    void open() override;
    double rate() const override { return rate_; }
    double lo_hz() const override { return lo_; }
    void write(const int16_t* iq, size_t samples) override;
    void close() override;
    void mute() noexcept override;

private:
    PlutoTxConfig cfg_;
    IiodClient iiod_;
    IioDeviceInfo phy_, dds_;
    bool connected_ = false, open_ = false;
    double rate_ = 0, lo_ = 0;
};

// Offline sink: writes the IQ to a file (cs16), for tests and for looking
// at the transmitted spectrum on a PC. With pace, write() waits like a DAC.
class FileTxSink : public TxSink {
public:
    FileTxSink(std::string path, double rate, double lo_hz, bool pace = false)
        : path_(std::move(path)), rate_(rate), lo_(lo_hz), pace_(pace) {}
    ~FileTxSink() override { close(); }
    void open() override;
    double rate() const override { return rate_; }
    double lo_hz() const override { return lo_; }
    void write(const int16_t* iq, size_t samples) override;
    void close() override;
    void mute() noexcept override { muted_ = true; }
    bool muted() const { return muted_; }

private:
    std::string path_;
    double rate_, lo_;
    bool pace_;
    FILE* f_ = nullptr;
    bool muted_ = false;
    std::chrono::steady_clock::time_point t0_;
    uint64_t written_ = 0;
};

}  // namespace nb
