#include "pluto_tx.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

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
bool is_loopback(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::string v;
    std::getline(f, v);
    return v;
}

// Throws with errno on failure (EBUSY when pluto_stream holds the buffer).
void write_sysfs(const std::string& path, const std::string& value) {
    const int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) throw IiodError("cannot open " + path + ": " + std::strerror(errno), -errno);
    const ssize_t n = ::write(fd, value.data(), value.size());
    const int err = errno;
    ::close(fd);
    if (n < 0) throw IiodError("cannot write " + value + " to " + path + ": " + std::strerror(err), -err);
}
}  // namespace

std::string find_local_iio_device(const std::string& name, const std::string& root) {
    DIR* d = ::opendir(root.c_str());
    if (!d) return {};
    std::string found;
    while (const dirent* e = ::readdir(d)) {
        if (std::strncmp(e->d_name, "iio:device", 10) != 0) continue;
        const std::string dir = root + "/" + e->d_name;
        if (read_file(dir + "/name") == name) {
            found = dir;
            break;
        }
    }
    ::closedir(d);
    return found;
}

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

    if (cfg_.local_dma && is_loopback(cfg_.host)) {
        sysfs_ = find_local_iio_device("cf-ad9361-dds-core-lpc");
        if (!sysfs_.empty() && ::access(("/dev/" + sysfs_.substr(sysfs_.rfind('/') + 1)).c_str(), W_OK) == 0) {
            open_local();
            open_ = true;
            return;
        }
        sysfs_.clear();
    }

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

void PlutoTx::open_local() {
    const std::string dev = "/dev/" + sysfs_.substr(sysfs_.rfind('/') + 1);
    // A buffer left enabled (by a crashed nbd) would refuse the settings.
    try {
        if (read_file(sysfs_ + "/buffer/enable") == "1") write_sysfs(sysfs_ + "/buffer/enable", "0");
    } catch (const IiodError& e) {
        if (e.code() == -EBUSY)
            throw IiodError("TX DMA busy: pluto_stream holds it (tx/stream/mode must be pass)", e.code());
        throw;
    }
    // I and Q of TX1 only.
    if (DIR* d = ::opendir((sysfs_ + "/scan_elements").c_str())) {
        while (const dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 3 && n.compare(n.size() - 3, 3, "_en") == 0 && n.compare(0, 11, "out_voltage") == 0)
                write_sysfs(sysfs_ + "/scan_elements/" + n,
                            n == "out_voltage0_en" || n == "out_voltage1_en" ? "1" : "0");
        }
        ::closedir(d);
    }
    // The DMA buffer's fileio mode splits the length into two blocks: two
    // blocks of kernel_buffers/2 nbd writes each, ~43 ms of slack.
    const size_t length = cfg_.buffer_samples * static_cast<size_t>(std::max(2, cfg_.kernel_buffers));
    write_sysfs(sysfs_ + "/buffer/length", std::to_string(length));
    fd_ = ::open(dev.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd_ < 0) throw IiodError("cannot open " + dev + ": " + std::strerror(errno), -errno);
    try {
        write_sysfs(sysfs_ + "/buffer/enable", "1");
    } catch (const IiodError& e) {
        ::close(fd_);
        fd_ = -1;
        if (e.code() == -EBUSY)
            throw IiodError("TX DMA busy: pluto_stream holds it (tx/stream/mode must be pass)", e.code());
        throw;
    }
}

void PlutoTx::close_local() noexcept {
    if (fd_ >= 0) {
        try {
            write_sysfs(sysfs_ + "/buffer/enable", "0");
        } catch (...) {
        }
        ::close(fd_);
        fd_ = -1;
    }
}

void PlutoTx::write(const int16_t* iq, size_t samples) {
    if (!open_) throw IiodError("TX buffer not open");
    if (fd_ >= 0) {
        const char* p = reinterpret_cast<const char*>(iq);
        size_t left = samples * 4;
        while (left > 0) {
            const ssize_t n = ::write(fd_, p, left);
            if (n < 0) {
                if (errno == EINTR) continue;
                throw IiodError(std::string("TX DMA write: ") + std::strerror(errno), -errno);
            }
            p += n;
            left -= static_cast<size_t>(n);
        }
        return;
    }
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
        if (fd_ >= 0) close_local();
        else iiod_.close_buffer(dds_);
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
