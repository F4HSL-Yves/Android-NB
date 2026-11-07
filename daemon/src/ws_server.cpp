#include "ws_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace nb {

namespace {

uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// Value of an HTTP header (case-insensitive name), trimmed.
std::string header(const std::string& head, const std::string& name) {
    const std::string lh = lower(head);
    const std::string key = "\r\n" + lower(name) + ":";
    const size_t p = lh.find(key);
    if (p == std::string::npos) return {};
    size_t s = p + key.size();
    size_t e = head.find("\r\n", s);
    if (e == std::string::npos) e = head.size();
    while (s < e && (head[s] == ' ' || head[s] == '\t')) ++s;
    while (e > s && (head[e - 1] == ' ' || head[e - 1] == '\t')) --e;
    return head.substr(s, e - s);
}

void set_nonblocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }

constexpr size_t kMaxMessage = 64 * 1024;

}  // namespace

std::string sha1_digest(const std::string& data) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string m = data;
    const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    m.push_back(static_cast<char>(0x80));
    while (m.size() % 64 != 56) m.push_back('\0');
    for (int i = 7; i >= 0; --i) m.push_back(static_cast<char>((bits >> (8 * i)) & 0xFF));
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<uint32_t>(static_cast<uint8_t>(m[off + 4 * i])) << 24) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(m[off + 4 * i + 1])) << 16) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(m[off + 4 * i + 2])) << 8) |
                   static_cast<uint32_t>(static_cast<uint8_t>(m[off + 4 * i + 3]));
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    std::string out(20, '\0');
    for (int i = 0; i < 20; ++i) out[i] = static_cast<char>((h[i / 4] >> (24 - 8 * (i % 4))) & 0xFF);
    return out;
}

std::string base64_encode(const std::string& data) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const uint32_t v = (static_cast<uint8_t>(data[i]) << 16) | (static_cast<uint8_t>(data[i + 1]) << 8) |
                           static_cast<uint8_t>(data[i + 2]);
        out += t[v >> 18], out += t[(v >> 12) & 63], out += t[(v >> 6) & 63], out += t[v & 63];
    }
    if (i + 1 == data.size()) {
        const uint32_t v = static_cast<uint8_t>(data[i]) << 16;
        out += t[v >> 18], out += t[(v >> 12) & 63], out += "==";
    } else if (i + 2 == data.size()) {
        const uint32_t v = (static_cast<uint8_t>(data[i]) << 16) | (static_cast<uint8_t>(data[i + 1]) << 8);
        out += t[v >> 18], out += t[(v >> 12) & 63], out += t[(v >> 6) & 63], out += '=';
    }
    return out;
}

std::string websocket_accept_key(const std::string& client_key) {
    return base64_encode(sha1_digest(client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
}

std::string websocket_frame(uint8_t opcode, const std::string& payload) {
    std::string f;
    f.push_back(static_cast<char>(0x80 | opcode));
    const size_t n = payload.size();
    if (n < 126) {
        f.push_back(static_cast<char>(n));
    } else if (n < 65536) {
        f.push_back(126);
        f.push_back(static_cast<char>(n >> 8));
        f.push_back(static_cast<char>(n & 0xFF));
    } else {
        f.push_back(127);
        for (int i = 7; i >= 0; --i) f.push_back(static_cast<char>((static_cast<uint64_t>(n) >> (8 * i)) & 0xFF));
    }
    return f + payload;
}

WsServer::~WsServer() { stop(); }

void WsServer::start(const std::string& bind_address, int port) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) throw std::runtime_error("socket failed");
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, bind_address.c_str(), &a.sin_addr) != 1)
        throw std::runtime_error("bad bind address " + bind_address);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(listen_fd_, 8) != 0)
        throw std::runtime_error("cannot listen on " + bind_address + ":" + std::to_string(port) + ": " +
                                 std::strerror(errno));
    socklen_t len = sizeof a;
    getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&a), &len);
    port_ = ntohs(a.sin_port);
    set_nonblocking(listen_fd_);
    if (::pipe(wake_fd_) != 0) throw std::runtime_error("pipe failed");
    set_nonblocking(wake_fd_[0]);
    set_nonblocking(wake_fd_[1]);
    stop_ = false;
    thread_ = std::thread([this] { run(); });
}

void WsServer::stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    wake();
    thread_.join();
    for (auto& c : list_) ::close(c.fd);
    list_.clear();
    clients_ = 0;
    ::close(listen_fd_);
    ::close(wake_fd_[0]);
    ::close(wake_fd_[1]);
    listen_fd_ = wake_fd_[0] = wake_fd_[1] = -1;
}

void WsServer::wake() {
    if (wake_fd_[1] < 0) return;
    const char b = 1;
    (void)!::write(wake_fd_[1], &b, 1);  // full pipe: a wake is already pending
}

void WsServer::set_hello(const std::string& text) {
    std::lock_guard<std::mutex> lk(mu_);
    hello_ = text;
}

void WsServer::broadcast_text(const std::string& text) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        pending_.push_back({false, websocket_frame(0x1, text)});
    }
    wake();
}

void WsServer::broadcast_audio(const std::string& payload) {
    if (clients_ == 0) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        pending_.push_back({true, websocket_frame(0x2, payload)});
    }
    wake();
}

bool WsServer::next_command(std::string& text) {
    std::lock_guard<std::mutex> lk(mu_);
    if (commands_.empty()) return false;
    text = std::move(commands_.front());
    commands_.pop_front();
    return true;
}

void WsServer::queue(Client& c, const Outgoing& o) {
    if (!c.open || c.closing) return;
    if (o.audio && c.out.size() > max_audio_backlog) {
        ++dropped_;
        return;
    }
    if (c.out.size() > max_text_backlog) {
        c.closing = true;
        c.out.clear();
        return;
    }
    c.out += o.frame;
}

void WsServer::run() {
    std::vector<pollfd> fds;
    while (!stop_) {
        fds.clear();
        fds.push_back({listen_fd_, POLLIN, 0});
        fds.push_back({wake_fd_[0], POLLIN, 0});
        for (auto& c : list_) fds.push_back({c.fd, static_cast<short>(POLLIN | (c.out.empty() ? 0 : POLLOUT)), 0});
        if (::poll(fds.data(), fds.size(), 1000) < 0 && errno != EINTR) break;
        if (stop_) break;
        if (fds[1].revents & POLLIN) {
            char buf[256];
            while (::read(wake_fd_[0], buf, sizeof buf) > 0) {
            }
        }
        std::vector<Outgoing> out;
        {
            std::lock_guard<std::mutex> lk(mu_);
            out.swap(pending_);
        }
        for (auto& c : list_)
            for (const auto& o : out) queue(c, o);

        // Client sockets: fds[2 + i] matches list_[i] as polled.
        const size_t polled = fds.size() - 2;
        for (size_t i = 0; i < polled && i < list_.size(); ++i) {
            Client& c = list_[i];
            bool ok = true;
            if (fds[2 + i].revents & (POLLIN | POLLHUP | POLLERR)) ok = read_client(c);
            if (ok && !c.out.empty()) ok = flush(c);
            if (ok && c.closing && c.out.empty()) ok = false;
            if (!ok) {
                ::close(c.fd);
                c.fd = -1;
            }
        }
        size_t open = 0;
        for (size_t i = 0; i < list_.size(); ++i)
            if (list_[i].fd >= 0) list_[open++] = std::move(list_[i]);
        list_.resize(open);
        if (fds[0].revents & POLLIN) accept_client();
        int n = 0;
        for (const auto& c : list_) n += c.open ? 1 : 0;
        clients_ = n;
    }
}

void WsServer::accept_client() {
    while (true) {
        const int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) return;
        set_nonblocking(fd);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        Client c;
        c.fd = fd;
        list_.push_back(std::move(c));
    }
}

bool WsServer::read_client(Client& c) {
    char buf[4096];
    while (true) {
        const ssize_t r = ::recv(c.fd, buf, sizeof buf, 0);
        if (r == 0) return false;
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return false;
        }
        c.in.append(buf, static_cast<size_t>(r));
        if (c.in.size() > 2 * kMaxMessage) return false;
    }
    if (!c.open && !handshake(c)) return false;
    return c.open ? parse_frames(c) : true;
}

bool WsServer::handshake(Client& c) {
    const size_t end = c.in.find("\r\n\r\n");
    if (end == std::string::npos) return c.in.size() < 8192;
    const std::string head = c.in.substr(0, end + 2);
    c.in.erase(0, end + 4);
    const std::string key = header(head, "Sec-WebSocket-Key");
    if (head.compare(0, 4, "GET ") != 0 || key.empty() || lower(header(head, "Upgrade")) != "websocket") {
        c.out = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
                "nbd: WebSocket only\n";
        c.closing = true;
        c.open = true;  // lets flush() send the reply; nothing else is queued
        return true;
    }
    c.out = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " +
            websocket_accept_key(key) + "\r\n\r\n";
    c.open = true;
    std::string hello;
    {
        std::lock_guard<std::mutex> lk(mu_);
        hello = hello_;
    }
    if (!hello.empty()) c.out += websocket_frame(0x1, hello);
    return true;
}

bool WsServer::parse_frames(Client& c) {
    while (c.in.size() >= 2) {
        const auto* p = reinterpret_cast<const uint8_t*>(c.in.data());
        const bool fin = p[0] & 0x80;
        const uint8_t op = p[0] & 0x0F;
        const bool masked = p[1] & 0x80;
        uint64_t len = p[1] & 0x7F;
        size_t pos = 2;
        if (len == 126) {
            if (c.in.size() < 4) return true;
            len = (static_cast<uint64_t>(p[2]) << 8) | p[3];
            pos = 4;
        } else if (len == 127) {
            if (c.in.size() < 10) return true;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | p[2 + i];
            pos = 10;
        }
        if (!masked || len > kMaxMessage) return false;  // RFC 6455: clients must mask
        if (c.in.size() < pos + 4 + len) return true;
        const uint8_t* mask = p + pos;
        std::string payload(static_cast<size_t>(len), '\0');
        for (size_t i = 0; i < len; ++i) payload[i] = static_cast<char>(p[pos + 4 + i] ^ mask[i % 4]);
        c.in.erase(0, pos + 4 + static_cast<size_t>(len));

        if (op == 0x8) {  // close: echo and finish
            c.out += websocket_frame(0x8, payload.substr(0, 2));
            c.closing = true;
            return true;
        }
        if (op == 0x9) {
            c.out += websocket_frame(0xA, payload);
            continue;
        }
        if (op == 0xA) continue;
        if (op == 0x1 || op == 0x2) {
            c.message_opcode = op;
            c.message = payload;
        } else if (op == 0x0) {
            c.message += payload;
            if (c.message.size() > kMaxMessage) return false;
        } else {
            return false;
        }
        if (fin) {
            if (c.message_opcode == 0x1) {
                std::lock_guard<std::mutex> lk(mu_);
                if (commands_.size() < 256) commands_.push_back(c.message);
            }
            c.message.clear();
        }
    }
    return true;
}

bool WsServer::flush(Client& c) {
    while (!c.out.empty()) {
        const ssize_t w = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return errno == EAGAIN || errno == EWOULDBLOCK;
        }
        c.out.erase(0, static_cast<size_t>(w));
    }
    return true;
}

}  // namespace nb
