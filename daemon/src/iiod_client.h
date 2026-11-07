// Minimal client for the IIOD text protocol (libiio 0.x network backend),
// the one the Pluto / Tezuka iiod speaks on TCP port 30431. Same commands as
// the Kotlin client of Android-DATV, so the behaviour on the board is known.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace nb {

class IiodError : public std::runtime_error {
public:
    IiodError(const std::string& what, int code = 0) : std::runtime_error(what), code_(code) {}
    int code() const { return code_; }

private:
    int code_;
};

struct IioChannelInfo {
    std::string id;
    bool output = false;
    int index_in_device = 0;  // position in the device's channel list (mask bit)
    bool scan_element = false;
};

struct IioDeviceInfo {
    std::string id;    // e.g. iio:device3
    std::string name;  // e.g. cf-ad9361-lpc
    std::vector<IioChannelInfo> channels;
    const IioChannelInfo* channel(const std::string& id, bool output) const;
};

// Parses the device / channel layout out of the context XML sent by PRINT.
std::vector<IioDeviceInfo> parse_iio_context(const std::string& xml);

// Hex mask word string as IIOD expects it ("00000003" for bits 0 and 1).
std::string iio_channel_mask(size_t channel_count, const std::vector<int>& enabled);

class IiodClient {
public:
    IiodClient() = default;
    ~IiodClient();
    IiodClient(const IiodClient&) = delete;
    IiodClient& operator=(const IiodClient&) = delete;

    void connect(const std::string& host, int port, int timeout_ms = 5000);
    void close();

    std::string version();
    std::vector<IioDeviceInfo> context();

    void write_channel_attr(const IioDeviceInfo& dev, const IioChannelInfo& ch, const std::string& attr,
                            const std::string& value);
    std::string read_channel_attr(const IioDeviceInfo& dev, const IioChannelInfo& ch, const std::string& attr);
    void write_debug_attr(const IioDeviceInfo& dev, const std::string& attr, const std::string& value);
    std::string read_debug_attr(const IioDeviceInfo& dev, const std::string& attr);

    // iio_device_reg_read / reg_write through direct_reg_access.
    uint32_t read_register(const IioDeviceInfo& dev, uint32_t address);
    void write_register(const IioDeviceInfo& dev, uint32_t address, uint32_t value);

    void set_kernel_buffers(const IioDeviceInfo& dev, int count);
    void open_buffer(const IioDeviceInfo& dev, size_t samples, const std::string& mask);
    // Reads up to out.size() bytes of samples; returns the byte count (0 at end).
    size_t read_buffer(const IioDeviceInfo& dev, std::vector<uint8_t>& out, size_t mask_words);
    void close_buffer(const IioDeviceInfo& dev);

private:
    void send(const std::string& s);
    void send_bytes(const void* data, size_t n);
    std::string read_line();
    int read_integer();
    void read_exact(void* data, size_t n);
    void expect_newline();

    int fd_ = -1;
    std::vector<uint8_t> rbuf_;
    size_t rpos_ = 0;
    size_t rlen_ = 0;
};

}  // namespace nb
