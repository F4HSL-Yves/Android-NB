#include "maia_waterfall.h"

#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "http_client.h"

namespace nb {

namespace {

// Reads exactly n bytes; false on stop request, timeout loops are retried.
bool read_exact(int fd, uint8_t* p, size_t n, const std::atomic<bool>& stop) {
    size_t got = 0;
    while (got < n) {
        if (stop) return false;
        const ssize_t r = ::recv(fd, p + got, n - got, 0);
        if (r > 0) {
            got += static_cast<size_t>(r);
            continue;
        }
        if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        throw std::runtime_error(r == 0 ? "maia closed the waterfall" : std::string("waterfall recv: ") + std::strerror(errno));
    }
    return true;
}

void send_all(int fd, const std::string& s) {
    size_t sent = 0;
    while (sent < s.size()) {
        const ssize_t w = ::send(fd, s.data() + sent, s.size() - sent, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) throw std::runtime_error("waterfall send failed");
        sent += static_cast<size_t>(w);
    }
}

// Client frames must be masked (RFC 6455 5.3); a zero mask is valid.
std::string client_frame(uint8_t opcode, const std::string& payload) {
    std::string f;
    f.push_back(static_cast<char>(0x80 | opcode));
    f.push_back(static_cast<char>(0x80 | payload.size()));  // control frames: < 126 bytes
    f.append(4, '\0');
    return f + payload;
}

}  // namespace

MaiaWaterfall::MaiaWaterfall(std::string host, int http_port) : host_(std::move(host)), port_(http_port) {}

MaiaWaterfall::~MaiaWaterfall() { stop(); }

void MaiaWaterfall::start() {
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread([this] { run(); });
}

void MaiaWaterfall::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void MaiaWaterfall::set_line_rate(double lines_per_s) { want_rate_ = lines_per_s; }

bool MaiaWaterfall::take_line(std::vector<float>& power) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!fresh_) return false;
    power = latest_;
    fresh_ = false;
    return true;
}

std::string MaiaWaterfall::last_error() const {
    std::lock_guard<std::mutex> lk(mu_);
    return error_;
}

void MaiaWaterfall::apply_rate() {
    const double r = want_rate_.load();
    if (r <= 0 || r == applied_rate_) return;
    char body[96];
    std::snprintf(body, sizeof body, "{\"output_sampling_frequency\":%.3f}", r);
    try {
        const HttpResponse resp = http_request(host_, port_, "PATCH", "/api/spectrometer", body, 3000);
        if (resp.status / 100 == 2) applied_rate_ = r;
    } catch (const std::exception&) {
        // Retried with the next line.
    }
}

void MaiaWaterfall::run() {
    while (!stop_) {
        try {
            session();
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(mu_);
            error_ = e.what();
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        applied_rate_ = 0.0;
        for (int i = 0; i < 20 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void MaiaWaterfall::session() {
    fd_ = tcp_connect(host_, port_, 3000);
    // Short receive timeout: the thread checks stop_ between reads.
    timeval tv{0, 300000};
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    send_all(fd_, "GET /waterfall HTTP/1.1\r\nHost: " + host_ +
                      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Key: bmJkLXdhdGVyZmFsbC0wMQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    std::string head;
    uint8_t c = 0;
    while (head.size() < 4 || head.compare(head.size() - 4, 4, "\r\n\r\n") != 0) {
        if (!read_exact(fd_, &c, 1, stop_)) return;
        head.push_back(static_cast<char>(c));
        if (head.size() > 4096) throw std::runtime_error("bad waterfall handshake");
    }
    if (head.compare(0, 12, "HTTP/1.1 101") != 0)
        throw std::runtime_error("maia refused the waterfall: " + head.substr(0, head.find('\r')));

    std::string message;
    std::vector<uint8_t> payload;
    while (!stop_) {
        apply_rate();
        uint8_t h[2];
        if (!read_exact(fd_, h, 2, stop_)) return;
        const uint8_t opcode = h[0] & 0x0F;
        const bool fin = h[0] & 0x80;
        uint64_t len = h[1] & 0x7F;
        if (h[1] & 0x80) throw std::runtime_error("masked frame from maia");
        if (len == 126 || len == 127) {
            uint8_t ext[8];
            const size_t k = len == 126 ? 2 : 8;
            if (!read_exact(fd_, ext, k, stop_)) return;
            len = 0;
            for (size_t i = 0; i < k; ++i) len = (len << 8) | ext[i];
        }
        if (len > (1u << 20)) throw std::runtime_error("waterfall frame too large");
        payload.resize(static_cast<size_t>(len));
        if (len && !read_exact(fd_, payload.data(), payload.size(), stop_)) return;
        if (opcode == 0x9) {  // ping
            send_all(fd_, client_frame(0xA, std::string(payload.begin(), payload.end())));
            continue;
        }
        if (opcode == 0x8) throw std::runtime_error("maia closed the waterfall");
        if (opcode != 0x2 && opcode != 0x0) continue;
        if (opcode == 0x2) message.clear();
        message.append(payload.begin(), payload.end());
        if (!fin) continue;
        const size_t n = message.size() / 4;
        if (n < 64) continue;
        std::vector<float> line(n);
        std::memcpy(line.data(), message.data(), n * 4);  // little endian on both ends
        line[0] = line[1];  // index 0 is a flag, not power
        {
            std::lock_guard<std::mutex> lk(mu_);
            latest_.swap(line);
            fresh_ = true;
            error_.clear();
        }
        ++lines_;
    }
}

}  // namespace nb
