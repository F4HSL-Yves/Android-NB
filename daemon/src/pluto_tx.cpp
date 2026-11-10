#include "pluto_tx.h"

#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <thread>

namespace nb {

namespace {
const IioDeviceInfo& find_device(const std::vector<IioDeviceInfo>& devs, const char* name) {
    for (const auto& d : devs)
        if (d.name == name) return d;
    throw IiodError(std::string("IIO device ") + name + " not found");
}

const IioChannelInfo& find_channel(const IioDeviceInfo& dev, const char* id, bool output) {
    const IioChannelInfo* c = dev.channel(id, output);
    if (!c) throw IiodError(std::string("channel ") + id + " not found on " + dev.name);
    return *c;
}
}  // namespace

PlutoTx::~PlutoTx() {
    mute();
    try {
        close();
    } catch (...) {
    }
}

void PlutoTx::open() {
    if (open_) return;
    if (!connected_) {
        iiod_.connect(cfg_.host, cfg_.iiod_port);
        connected_ = true;
        const auto devs = iiod_.context();
        phy_ = find_device(devs, "ad9361-phy");
        dds_ = find_device(devs, "cf-ad9361-dds-core-lpc");
    }
    mute();
    const auto& tx = find_channel(phy_, "voltage0", true);
    const auto& lo = find_channel(phy_, "altvoltage1", true);
    rate_ = std::atof(iiod_.read_channel_attr(phy_, tx, "sampling_frequency").c_str());
    if (std::fabs(rate_ - cfg_.expected_rate) > cfg_.expected_rate * 1e-3)
        throw IiodError("the AD9363 runs at " + std::to_string(static_cast<long>(rate_)) +
                        " S/s, not 3.072 MS/s: is a DATV transmission holding the clock?");
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f", cfg_.lo_hz);
    iiod_.write_channel_attr(phy_, lo, "frequency", buf);
    lo_ = std::atof(iiod_.read_channel_attr(phy_, lo, "frequency").c_str());
    if (std::fabs(lo_ - cfg_.lo_hz) > 1000.0)
        throw IiodError("the AD9363 refused TX LO " + std::string(buf) + " Hz");

    iiod_.set_kernel_buffers(dds_, cfg_.kernel_buffers);
    const auto& i_ch = find_channel(dds_, "voltage0", true);
    const auto& q_ch = find_channel(dds_, "voltage1", true);
    const std::string mask = iio_channel_mask(dds_.channels.size(), {i_ch.index_in_device, q_ch.index_in_device});
    try {
        iiod_.open_buffer(dds_, cfg_.buffer_samples, mask);
    } catch (const IiodError& e) {
        if (e.code() == -16)
            throw IiodError("TX DMA busy: pluto_stream holds it (tx/stream/mode must be pass)", e.code());
        throw;
    }
    open_ = true;
}

void PlutoTx::write(const int16_t* iq, size_t samples) {
    if (!open_) throw IiodError("TX buffer not open");
    // iiod pushes a DMA block each time buffer_samples are written.
    const size_t block = cfg_.buffer_samples;
    size_t pos = 0;
    while (pos < samples) {
        const size_t n = std::min(block, samples - pos);
        iiod_.write_buffer(dds_, iq + 2 * pos, n * 4);
        pos += n;
    }
}

void PlutoTx::close() {
    if (open_) {
        open_ = false;
        iiod_.close_buffer(dds_);
    }
}

void PlutoTx::mute() noexcept {
    if (!connected_) return;
    try {
        const auto& lo = find_channel(phy_, "altvoltage1", true);
        iiod_.write_channel_attr(phy_, lo, "powerdown", "1");
    } catch (...) {
        // The connection is gone: try once on a fresh one, RF must go off.
        try {
            IiodClient c;
            c.connect(cfg_.host, cfg_.iiod_port, 2000);
            const auto& lo = find_channel(phy_, "altvoltage1", true);
            c.write_channel_attr(phy_, lo, "powerdown", "1");
        } catch (...) {
        }
    }
}

void FileTxSink::open() {
    if (f_) return;
    f_ = std::fopen(path_.c_str(), "wb");
    if (!f_) throw std::runtime_error("cannot open " + path_);
    muted_ = true;
    t0_ = std::chrono::steady_clock::now();
    written_ = 0;
}

void FileTxSink::write(const int16_t* iq, size_t samples) {
    if (f_) std::fwrite(iq, sizeof(int16_t), 2 * samples, f_);
    written_ += samples;
    if (pace_)
        std::this_thread::sleep_until(t0_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                std::chrono::duration<double>(static_cast<double>(written_) / rate_)));
}

void FileTxSink::close() {
    if (f_) std::fclose(f_);
    f_ = nullptr;
}

}  // namespace nb
