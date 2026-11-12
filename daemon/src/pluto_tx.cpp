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
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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
    // Undo what another program (SDR Console) may have left on the TX
    // path: a FIR designed for its own rate, and a narrow analog filter.
    // Our signal lies 0.5 to 1 MHz above the LO; either one attenuates it.
    // The FIR is not needed at 3.072 MS/s (the half-band chain does it).
    // The receiver already did this at start; here only if something
    // turned the FIR on since. Then the clock chain must be rebuilt (a
    // brief glitch in reception), or it no longer matches our samples.
    try {
        if (iiod_.read_channel_attr(phy_, tx, "filter_fir_en") != "0") {
            iiod_.write_channel_attr(phy_, tx, "filter_fir_en", "0");
            const std::string want = iiod_.read_channel_attr(phy_, tx, "sampling_frequency");
            // 2.5 MS/s: another rate above the 2.08 MS/s floor without FIR.
            iiod_.write_channel_attr(phy_, tx, "sampling_frequency", "2500000");
            iiod_.write_channel_attr(phy_, tx, "sampling_frequency", want);
        }
    } catch (const IiodError&) {
    }
    iiod_.write_channel_attr(phy_, tx, "rf_bandwidth", std::to_string(cfg_.rf_bandwidth));
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

    // The DAC path of the FPGA has an x8 interpolator (for rates below
    // ~520 kS/s). Something on the board (pluto_stream's DATV setup) can
    // leave it on behind the driver's back: the attribute still reads
    // 3071999 while the DMA drains at 384 kS/s (measured on the FishBall).
    // Writing the low rate then the full one makes the driver program it
    // again; the Transmitter checks the real pace anyway.
    {
        const auto& d0 = find_channel(dds_, "voltage0", true);
        char lo_rate[32], full[32];
        std::snprintf(lo_rate, sizeof lo_rate, "%.0f", std::floor(rate_ / 8.0));
        std::snprintf(full, sizeof full, "%.0f", std::floor(rate_));
        try {
            iiod_.write_channel_attr(dds_, d0, "sampling_frequency", lo_rate);
        } catch (const IiodError&) {
        }
        iiod_.write_channel_attr(dds_, d0, "sampling_frequency", full);
    }

    if (cfg_.local_dma && is_loopback(cfg_.host)) {
        sysfs_ = find_local_iio_device("cf-ad9361-dds-core-lpc");
        if (!sysfs_.empty() && ::access(("/dev/" + sysfs_.substr(sysfs_.rfind('/') + 1)).c_str(), W_OK) == 0 &&
            open_local()) {
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

namespace {
// ADI's IIO DMA block interface (include/uapi/linux/iio/buffer.h in the ADI
// kernel; libiio local.c uses the same definitions).
struct IioBlockAllocReq {
    uint32_t type, size, count, id;
};
struct IioBlock {
    uint32_t id, size, bytes_used, type, flags, offset;
    uint64_t timestamp;
};
constexpr unsigned long kBlockAlloc = _IOWR('i', 0xa0, IioBlockAllocReq);
constexpr unsigned long kBlockFree = _IO('i', 0xa1);
constexpr unsigned long kBlockQuery = _IOWR('i', 0xa2, IioBlock);
constexpr unsigned long kBlockEnqueue = _IOWR('i', 0xa3, IioBlock);
constexpr unsigned long kBlockDequeue = _IOWR('i', 0xa4, IioBlock);
constexpr int kDacTimeoutMs = 1000;

int ioctl_retry(int fd, unsigned long req, void* arg) {
    int r;
    do r = ::ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

IiodError busy_or(const IiodError& e) {
    if (e.code() == -EBUSY) return IiodError("TX DMA busy: pluto_stream holds it (tx/stream/mode must be pass)", e.code());
    return e;
}
}  // namespace

bool PlutoTx::open_local() {
    const std::string dev = "/dev/" + sysfs_.substr(sysfs_.rfind('/') + 1);
    // A buffer left enabled (by a crashed nbd) would refuse the settings.
    try {
        if (read_file(sysfs_ + "/buffer/enable") == "1") write_sysfs(sysfs_ + "/buffer/enable", "0");
    } catch (const IiodError& e) {
        throw busy_or(e);
    }
    // I and Q of TX1 only.
    if (DIR* d = ::opendir((sysfs_ + "/scan_elements").c_str())) {
        std::vector<std::string> names;
        while (const dirent* e = ::readdir(d)) names.emplace_back(e->d_name);
        ::closedir(d);
        for (const auto& n : names)
            if (n.size() > 3 && n.compare(n.size() - 3, 3, "_en") == 0 && n.compare(0, 11, "out_voltage") == 0)
                write_sysfs(sysfs_ + "/scan_elements/" + n, n == "out_voltage0_en" || n == "out_voltage1_en" ? "1" : "0");
    }
    fd_ = ::open(dev.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd_ < 0) throw IiodError("cannot open " + dev + ": " + std::strerror(errno), -errno);

    const unsigned count = static_cast<unsigned>(std::max(2, cfg_.kernel_buffers));
    IioBlockAllocReq req{0, static_cast<uint32_t>(cfg_.buffer_samples * 4), count, 0};
    if (ioctl_retry(fd_, kBlockAlloc, &req) < 0) {
        const int err = errno;
        ::close(fd_);
        fd_ = -1;
        if (err == EBUSY) throw busy_or(IiodError("block alloc", -EBUSY));
        return false;  // no block interface in this kernel
    }
    try {
        if (req.count < 2) throw IiodError("TX DMA: the kernel gave " + std::to_string(req.count) + " block(s)");
        for (uint32_t i = 0; i < req.count; ++i) {
            IioBlock b{};
            b.id = i;
            if (ioctl_retry(fd_, kBlockQuery, &b) < 0)
                throw IiodError(std::string("TX DMA block query: ") + std::strerror(errno), -errno);
            void* m = ::mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, b.offset);
            if (m == MAP_FAILED) throw IiodError(std::string("TX DMA mmap: ") + std::strerror(errno), -errno);
            std::memset(m, 0, b.size);
            maps_.push_back(m);
            block_size_.push_back(b.size);
            fresh_.push_back(i);
        }
        write_sysfs(sysfs_ + "/buffer/enable", "1");
    } catch (const IiodError& e) {
        close_local();
        throw busy_or(e);
    }
    have_cur_ = false;
    return true;
}

void PlutoTx::close_local() noexcept {
    if (fd_ < 0) return;
    try {
        write_sysfs(sysfs_ + "/buffer/enable", "0");
    } catch (...) {
    }
    for (size_t i = 0; i < maps_.size(); ++i) ::munmap(maps_[i], block_size_[i]);
    maps_.clear();
    block_size_.clear();
    fresh_.clear();
    ioctl_retry(fd_, kBlockFree, nullptr);
    ::close(fd_);
    fd_ = -1;
    have_cur_ = false;
}

// Makes cur_ a block that is free to fill: one never queued, else the next
// one the DMA has finished with (waits up to kDacTimeoutMs for it).
void PlutoTx::next_block() {
    if (!fresh_.empty()) {
        cur_ = fresh_.front();
        fresh_.erase(fresh_.begin());
    } else {
        // poll() may report room before a block is really back: retry the
        // dequeue until the deadline.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kDacTimeoutMs);
        IioBlock b{};
        while (true) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) throw IiodError("TX DMA stalled: no block back from the DAC in 1 s");
            pollfd p{fd_, POLLOUT, 0};
            const int r = ::poll(&p, 1, static_cast<int>(left));
            if (r < 0 && errno != EINTR) throw IiodError(std::string("TX DMA poll: ") + std::strerror(errno), -errno);
            if (r <= 0) continue;
            b = IioBlock{};
            if (ioctl_retry(fd_, kBlockDequeue, &b) == 0) break;
            if (errno != EAGAIN && errno != EBUSY)
                throw IiodError(std::string("TX DMA dequeue: ") + std::strerror(errno), -errno);
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        if (b.id >= maps_.size()) throw IiodError("TX DMA dequeue: bad block id");
        cur_ = b.id;
    }
    cur_used_ = 0;
    have_cur_ = true;
}

void PlutoTx::write(const int16_t* iq, size_t samples) {
    if (!open_) throw IiodError("TX buffer not open");
    if (fd_ >= 0) {
        const char* p = reinterpret_cast<const char*>(iq);
        size_t left = samples * 4;
        while (left > 0) {
            if (!have_cur_) next_block();
            const size_t room = block_size_[cur_] - cur_used_;
            const size_t n = std::min(room, left);
            std::memcpy(static_cast<char*>(maps_[cur_]) + cur_used_, p, n);
            cur_used_ += n;
            p += n;
            left -= n;
            if (cur_used_ == block_size_[cur_]) {
                IioBlock b{};
                b.id = cur_;
                b.size = block_size_[cur_];
                b.bytes_used = block_size_[cur_];
                if (ioctl_retry(fd_, kBlockEnqueue, &b) < 0)
                    throw IiodError(std::string("TX DMA enqueue: ") + std::strerror(errno), -errno);
                have_cur_ = false;
            }
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
