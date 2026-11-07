#include "http_client.h"

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace nb {

namespace {

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string dechunk(const std::string& body) {
    std::string out;
    size_t pos = 0;
    while (pos < body.size()) {
        const size_t eol = body.find("\r\n", pos);
        if (eol == std::string::npos) break;
        const long size = std::strtol(body.substr(pos, eol - pos).c_str(), nullptr, 16);
        if (size <= 0) break;
        const size_t start = eol + 2;
        if (start + static_cast<size_t>(size) > body.size()) break;
        out.append(body, start, static_cast<size_t>(size));
        pos = start + static_cast<size_t>(size) + 2;
    }
    return out;
}

}  // namespace

int tcp_connect(const std::string& host, int port, int timeout_ms) {
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
        throw std::runtime_error("cannot resolve " + host);
    int fd = -1;
    for (addrinfo* a = res; a; a = a->ai_next) {
        fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) throw std::runtime_error("cannot connect to " + host + ":" + service);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    return fd;
}

HttpResponse http_request(const std::string& host, int port, const std::string& method, const std::string& path,
                          const std::string& json_body, int timeout_ms) {
    const int fd = tcp_connect(host, port, timeout_ms);
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + host + "\r\nAccept: application/json\r\n";
    if (!json_body.empty()) req += "Content-Type: application/json\r\n";
    req += "Content-Length: " + std::to_string(json_body.size()) + "\r\nConnection: close\r\n\r\n" + json_body;
    size_t sent = 0;
    while (sent < req.size()) {
        const ssize_t w = ::send(fd, req.data() + sent, req.size() - sent, MSG_NOSIGNAL);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            ::close(fd);
            throw std::runtime_error("http send failed");
        }
        sent += static_cast<size_t>(w);
    }
    std::string raw;
    char buf[4096];
    while (raw.size() < 1024 * 1024) {
        const ssize_t r = ::recv(fd, buf, sizeof buf, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        raw.append(buf, static_cast<size_t>(r));
    }
    ::close(fd);

    const size_t hend = raw.find("\r\n\r\n");
    if (hend == std::string::npos) throw std::runtime_error("incomplete http response");
    HttpResponse resp;
    const std::string head = raw.substr(0, hend);
    if (head.compare(0, 5, "HTTP/") != 0) throw std::runtime_error("bad http status line");
    const size_t sp = head.find(' ');
    resp.status = sp == std::string::npos ? 0 : std::atoi(head.c_str() + sp + 1);
    resp.body = raw.substr(hend + 4);
    const std::string lhead = lower(head);
    if (lhead.find("transfer-encoding: chunked") != std::string::npos) resp.body = dechunk(resp.body);
    return resp;
}

bool json_number(const std::string& json, const std::string& key, double& value) {
    const std::string k = "\"" + key + "\"";
    size_t p = json.find(k);
    if (p == std::string::npos) return false;
    p = json.find(':', p + k.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r')) ++p;
    char* end = nullptr;
    const double v = std::strtod(json.c_str() + p, &end);
    if (end == json.c_str() + p) return false;
    value = v;
    return true;
}

}  // namespace nb
