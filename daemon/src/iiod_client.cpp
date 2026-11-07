#include "iiod_client.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nb {

namespace {

std::string attr_value(const std::string& tag, const char* name) {
    const std::string key = std::string(" ") + name + "=\"";
    const size_t p = tag.find(key);
    if (p == std::string::npos) return {};
    const size_t start = p + key.size();
    const size_t end = tag.find('"', start);
    return end == std::string::npos ? std::string() : tag.substr(start, end - start);
}

std::string clean(std::string s) {
    while (!s.empty() && (s.back() == '\0' || s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\0')) ++i;
    return s.substr(i);
}

std::string hex32(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%X", v);
    return buf;
}

}  // namespace

const IioChannelInfo* IioDeviceInfo::channel(const std::string& cid, bool output) const {
    for (const auto& c : channels)
        if (c.id == cid && c.output == output) return &c;
    return nullptr;
}

std::vector<IioDeviceInfo> parse_iio_context(const std::string& xml) {
    std::vector<IioDeviceInfo> devices;
    size_t pos = 0;
    while (true) {
        const size_t lt = xml.find('<', pos);
        if (lt == std::string::npos) break;
        const size_t gt = xml.find('>', lt);
        if (gt == std::string::npos) break;
        const std::string tag = xml.substr(lt, gt - lt + 1);
        pos = gt + 1;
        if (tag.compare(0, 8, "<device ") == 0) {
            IioDeviceInfo d;
            d.id = attr_value(tag, "id");
            d.name = attr_value(tag, "name");
            devices.push_back(d);
        } else if (tag.compare(0, 9, "<channel ") == 0 && !devices.empty()) {
            IioChannelInfo c;
            c.id = attr_value(tag, "id");
            c.output = attr_value(tag, "type") == "output";
            c.index_in_device = static_cast<int>(devices.back().channels.size());
            devices.back().channels.push_back(c);
        } else if (tag.compare(0, 14, "<scan-element ") == 0 && !devices.empty() &&
                   !devices.back().channels.empty()) {
            devices.back().channels.back().scan_element = true;
        }
    }
    return devices;
}

std::string iio_channel_mask(size_t channel_count, const std::vector<int>& enabled) {
    std::vector<uint32_t> words((channel_count + 31) / 32, 0u);
    for (int i : enabled) {
        if (i < 0 || static_cast<size_t>(i) >= channel_count) throw IiodError("channel index out of range");
        words[i / 32] |= 1u << (i % 32);
    }
    std::string out;
    for (size_t w = words.size(); w-- > 0;) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%08x", words[w]);
        out += buf;
    }
    return out;
}

IiodClient::~IiodClient() { close(); }

void IiodClient::connect(const std::string& host, int port, int timeout_ms) {
    close();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string service = std::to_string(port);
    // Numeric addresses first: they need no NSS module, which a static
    // glibc binary cannot load on the board.
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &res) != 0) {
        res = nullptr;
        hints.ai_flags = AI_NUMERICSERV;
    }
    if (!res && (getaddrinfo(host.c_str(), service.c_str(), &hints, &res) != 0 || !res))
        throw IiodError("cannot resolve " + host);
    int fd = -1;
    for (addrinfo* a = res; a; a = a->ai_next) {
        fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) throw IiodError("cannot connect to iiod at " + host + ":" + service);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    int big = 2 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    fd_ = fd;
    rbuf_.assign(65536, 0);
    rpos_ = rlen_ = 0;
}

void IiodClient::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

void IiodClient::send_bytes(const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    while (n > 0) {
        const ssize_t w = ::send(fd_, p, n, MSG_NOSIGNAL);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            throw IiodError("iiod connection lost while sending");
        }
        p += w;
        n -= static_cast<size_t>(w);
    }
}

void IiodClient::send(const std::string& s) { send_bytes(s.data(), s.size()); }

void IiodClient::read_exact(void* data, size_t n) {
    auto* p = static_cast<uint8_t*>(data);
    while (n > 0) {
        if (rpos_ < rlen_) {
            const size_t take = std::min(n, rlen_ - rpos_);
            std::memcpy(p, &rbuf_[rpos_], take);
            rpos_ += take;
            p += take;
            n -= take;
            continue;
        }
        if (n >= rbuf_.size()) {  // large reads go straight to the caller's buffer
            const ssize_t r = ::recv(fd_, p, n, 0);
            if (r <= 0) {
                if (r < 0 && errno == EINTR) continue;
                throw IiodError("iiod connection lost while reading");
            }
            p += r;
            n -= static_cast<size_t>(r);
            continue;
        }
        const ssize_t r = ::recv(fd_, rbuf_.data(), rbuf_.size(), 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            throw IiodError("iiod connection lost while reading");
        }
        rpos_ = 0;
        rlen_ = static_cast<size_t>(r);
    }
}

std::string IiodClient::read_line() {
    std::string line;
    char c = 0;
    while (line.size() < 4096) {
        read_exact(&c, 1);
        if (c == '\n') return line;
        if (c != '\r') line.push_back(c);
    }
    throw IiodError("iiod line too long");
}

int IiodClient::read_integer() {
    for (int i = 0; i < 16; ++i) {
        const std::string line = clean(read_line());
        if (line.empty()) continue;
        char* end = nullptr;
        const long v = std::strtol(line.c_str(), &end, 10);
        if (end == line.c_str()) throw IiodError("unexpected iiod reply: " + line);
        return static_cast<int>(v);
    }
    throw IiodError("empty iiod reply");
}

void IiodClient::expect_newline() {
    char c = 0;
    read_exact(&c, 1);
    if (c != '\n') throw IiodError("malformed iiod reply");
}

std::string IiodClient::version() {
    send("VERSION\r\n");
    return clean(read_line());
}

std::vector<IioDeviceInfo> IiodClient::context() {
    send("PRINT\r\n");
    const int len = read_integer();
    if (len < 0 || len > 4 * 1024 * 1024) throw IiodError("PRINT refused", len);
    std::string xml(static_cast<size_t>(len), '\0');
    read_exact(&xml[0], xml.size());
    expect_newline();
    return parse_iio_context(xml);
}

void IiodClient::write_channel_attr(const IioDeviceInfo& dev, const IioChannelInfo& ch, const std::string& attr,
                                    const std::string& value) {
    send("WRITE " + dev.id + (ch.output ? " OUTPUT " : " INPUT ") + ch.id + " " + attr + " " +
         std::to_string(value.size()) + "\r\n");
    send(value);
    const int r = read_integer();
    if (r < 0) throw IiodError("writing " + ch.id + "/" + attr + " refused", r);
}

std::string IiodClient::read_channel_attr(const IioDeviceInfo& dev, const IioChannelInfo& ch, const std::string& attr) {
    send("READ " + dev.id + (ch.output ? " OUTPUT " : " INPUT ") + ch.id + " " + attr + "\r\n");
    const int len = read_integer();
    if (len < 0 || len > 4096) throw IiodError("reading " + ch.id + "/" + attr + " refused", len);
    std::string v(static_cast<size_t>(len), '\0');
    if (len) read_exact(&v[0], v.size());
    expect_newline();
    return clean(v);
}

void IiodClient::write_debug_attr(const IioDeviceInfo& dev, const std::string& attr, const std::string& value) {
    send("WRITE " + dev.id + " DEBUG " + attr + " " + std::to_string(value.size()) + "\r\n");
    send(value);
    const int r = read_integer();
    if (r < 0) throw IiodError("writing debug " + attr + " refused", r);
}

std::string IiodClient::read_debug_attr(const IioDeviceInfo& dev, const std::string& attr) {
    send("READ " + dev.id + " DEBUG " + attr + "\r\n");
    const int len = read_integer();
    if (len < 0 || len > 4096) throw IiodError("reading debug " + attr + " refused", len);
    std::string v(static_cast<size_t>(len), '\0');
    if (len) read_exact(&v[0], v.size());
    expect_newline();
    return clean(v);
}

uint32_t IiodClient::read_register(const IioDeviceInfo& dev, uint32_t address) {
    write_debug_attr(dev, "direct_reg_access", hex32(address));
    const std::string v = read_debug_attr(dev, "direct_reg_access");
    char* end = nullptr;
    const unsigned long r = std::strtoul(v.c_str(), &end, 0);
    if (end == v.c_str()) throw IiodError("unreadable register value: " + v);
    return static_cast<uint32_t>(r);
}

void IiodClient::write_register(const IioDeviceInfo& dev, uint32_t address, uint32_t value) {
    write_debug_attr(dev, "direct_reg_access", hex32(address) + " " + hex32(value));
}

void IiodClient::set_kernel_buffers(const IioDeviceInfo& dev, int count) {
    send("SET " + dev.id + " BUFFERS_COUNT " + std::to_string(count) + "\r\n");
    read_integer();  // older iiod versions may refuse; harmless
}

void IiodClient::open_buffer(const IioDeviceInfo& dev, size_t samples, const std::string& mask) {
    send("OPEN " + dev.id + " " + std::to_string(samples) + " " + mask + "\r\n");
    const int r = read_integer();
    if (r == -16) throw IiodError("RX buffer busy: another program (zynqrx, maia recorder...) holds it", r);
    if (r < 0) throw IiodError("OPEN " + dev.id + " refused", r);
}

size_t IiodClient::read_buffer(const IioDeviceInfo& dev, std::vector<uint8_t>& out, size_t mask_words) {
    send("READBUF " + dev.id + " " + std::to_string(out.size()) + "\r\n");
    size_t offset = 0;
    bool first = true;
    while (offset < out.size()) {
        const int len = read_integer();
        if (len < 0) throw IiodError("READBUF refused", len);
        if (len == 0) break;
        if (first) {
            std::string mask(mask_words * 8 + 1, '\0');
            read_exact(&mask[0], mask.size());
            if (mask.back() != '\n') throw IiodError("malformed READBUF mask");
            first = false;
        }
        if (static_cast<size_t>(len) > out.size() - offset) throw IiodError("READBUF block larger than requested");
        read_exact(out.data() + offset, static_cast<size_t>(len));
        offset += static_cast<size_t>(len);
    }
    return offset;
}

void IiodClient::close_buffer(const IioDeviceInfo& dev) {
    send("CLOSE " + dev.id + "\r\n");
    const int r = read_integer();
    if (r < 0) throw IiodError("CLOSE refused", r);
}

}  // namespace nb
