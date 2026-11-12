// Transmit output of nbd: the DAC side of the AD9363.
//
// PlutoTx streams int16 IQ to cf-ad9361-dds-core-lpc through iiod (its own
// TCP connection, the receive one stays busy reading). The AD9363 shares its
// sample rate between RX and TX, so the DAC runs at the 3.072 MS/s nbd set
// for reception; PlutoTx only checks it. It sets the TX LO (below the
// transponder) and can cut the RF at once (TX LO power down), which is
// what Tezuka's tx/mute does.
//
// On the board itself (iiod host 127.0.0.1) the samples bypass iiod: its
// WRITEBUF path proved far too slow for 12 MB/s (about 1/8 of real time on
// the FishBall), so PlutoTx feeds /dev/iio:deviceN directly through the
// ADI block interface (mmap'ed DMA blocks, as libiio's local backend does;
// plain write() spins in this kernel). iiod still does the attributes and
// the mute. Without the block interface it falls back to iiod.
//
// pluto_stream must leave the DMA free: its tx/stream/mode has to be "pass"
// (the app sets it over MQTT before the first transmission).
#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

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
    // How the samples go out ("dma", "iiod", "file"), for the status.
    virtual const char* kind() const { return ""; }
};

struct PlutoTxConfig {
    std::string host = "127.0.0.1";
    int iiod_port = 30431;
    double lo_hz = 2399500000.0;   // below the NB uplink (2400.000 - 2400.500 MHz)
    double expected_rate = 3072000.0;
    // Analog TX bandwidth: the signal sits 0.5 to 1 MHz above the LO, so
    // another program's narrow setting (SDR Console leaves 1.152 MHz, about
    // +-576 kHz) would cut it. 18 MHz is the board's device-tree default.
    long rf_bandwidth = 18000000;
    size_t buffer_samples = 32768;  // ~10.7 ms per DMA block
    int kernel_buffers = 4;
    // Direct /dev/iio write when iiod is local and the device node exists.
    bool local_dma = true;
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
    const char* kind() const override { return fd_ >= 0 ? "dma" : "iiod"; }

private:
    PlutoTxConfig cfg_;
    IiodClient iiod_;
    IioDeviceInfo phy_, dds_;
    bool connected_ = false, open_ = false;
    double rate_ = 0, lo_ = 0;
    // Local DMA path: sysfs directory of the dds core and its /dev node.
    std::string sysfs_;
    int fd_ = -1;
    std::vector<void*> maps_;
    std::vector<uint32_t> block_size_;
    // Blocks never handed to the DMA yet (filled first, without a dequeue).
    std::vector<uint32_t> fresh_;
    uint32_t cur_ = 0;        // block being filled
    size_t cur_used_ = 0;     // bytes already in it
    bool have_cur_ = false;
    bool open_local();        // false: no block interface, use iiod
    void close_local() noexcept;
    void next_block();
};

// "/sys/bus/iio/devices/iio:deviceN" of the device called name, or "".
std::string find_local_iio_device(const std::string& name, const std::string& root = "/sys/bus/iio/devices");

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
    const char* kind() const override { return "file"; }
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
