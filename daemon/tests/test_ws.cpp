// WebSocket server, protocol and Opus tests, with a raw socket client.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <opus/opus.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <functional>
#include <atomic>
#include <cmath>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "maia_waterfall.h"
#include "opus_audio.h"
#include "protocol.h"
#include "ws_server.h"

namespace {

int failures = 0;
#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                          \
            std::printf("\n");                                 \
            ++failures;                                        \
        }                                                      \
    } while (0)

struct Client {
    int fd = -1;
    std::string in;

    explicit Client(int port, bool upgrade = true) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<uint16_t>(port));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) std::perror("connect");
        std::string req = "GET / HTTP/1.1\r\nHost: x\r\n";
        if (upgrade)
            req += "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                   "Sec-WebSocket-Version: 13\r\n";
        req += "\r\n";
        ::send(fd, req.data(), req.size(), 0);
    }
    ~Client() { ::close(fd); }

    bool fill(int timeout_ms) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, timeout_ms) <= 0) return false;
        char buf[8192];
        const ssize_t r = ::recv(fd, buf, sizeof buf, 0);
        if (r <= 0) return false;
        in.append(buf, static_cast<size_t>(r));
        return true;
    }
    std::string head(int timeout_ms = 2000) {
        while (in.find("\r\n\r\n") == std::string::npos)
            if (!fill(timeout_ms)) return {};
        const size_t e = in.find("\r\n\r\n");
        std::string h = in.substr(0, e);
        in.erase(0, e + 4);
        return h;
    }
    // Next server frame; opcode 0 on timeout.
    bool frame(uint8_t& op, std::string& payload, int timeout_ms = 2000) {
        while (true) {
            if (in.size() >= 2) {
                const auto* p = reinterpret_cast<const uint8_t*>(in.data());
                size_t len = p[1] & 0x7F, pos = 2;
                if (len == 126) {
                    if (in.size() < 4) goto more;
                    len = (p[2] << 8) | p[3];
                    pos = 4;
                }
                if (in.size() >= pos + len) {
                    op = p[0] & 0x0F;
                    payload = in.substr(pos, len);
                    in.erase(0, pos + len);
                    return true;
                }
            }
        more:
            if (!fill(timeout_ms)) {
                op = 0;
                return false;
            }
        }
    }
    void send_frame(uint8_t op, const std::string& payload) {
        std::string f;
        f.push_back(static_cast<char>(0x80 | op));
        f.push_back(static_cast<char>(0x80 | payload.size()));  // < 126 in these tests
        const char mask[4] = {0x12, 0x34, 0x56, 0x78};
        f.append(mask, 4);
        for (size_t i = 0; i < payload.size(); ++i) f.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
        ::send(fd, f.data(), f.size(), 0);
    }
};

bool wait_for(const std::function<bool()>& cond) {
    for (int i = 0; i < 200; ++i) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

void test_helpers() {
    std::printf("helpers\n");
    // RFC 6455 section 1.3 example.
    CHECK(nb::websocket_accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "accept key");
    CHECK(nb::base64_encode("ab") == "YWI=" && nb::base64_encode("abc") == "YWJj" && nb::base64_encode("a") == "YQ==",
          "base64");
    const std::string big(300, 'x');
    const std::string f = nb::websocket_frame(0x2, big);
    CHECK(f.size() == 304 && static_cast<uint8_t>(f[1]) == 126 && static_cast<uint8_t>(f[2]) == 1 &&
              static_cast<uint8_t>(f[3]) == 44,
          "16-bit length frame");

    std::string err;
    auto c = nb::parse_command("{\"cmd\":\"freq\",\"hz\":10489900000}", err);
    CHECK(c.type == nb::Command::Freq && c.a == 10489900000.0, "freq");
    c = nb::parse_command("{\"cmd\": \"filter\", \"low\": 300, \"high\": 2700}", err);
    CHECK(c.type == nb::Command::Filter && c.a == 300 && c.b == 2700, "filter");
    err.clear();
    c = nb::parse_command("{\"cmd\":\"filter\",\"low\":2700,\"high\":300}", err);
    CHECK(c.type == nb::Command::None && !err.empty(), "bad filter refused");
    err.clear();
    c = nb::parse_command("{\"cmd\":\"ptt\"}", err);
    CHECK(c.type == nb::Command::None && err == "unknown cmd ptt", "unknown: %s", err.c_str());
    err.clear();
    c = nb::parse_command("{\"cmd\":\"tx\"}", err);
    CHECK(c.type == nb::Command::None && err == "tx: missing on", "tx without on: %s", err.c_str());
    const std::string pkt = nb::audio_packet(0x1234, "abc");
    CHECK(pkt.size() == 6 && pkt[0] == 1 && static_cast<uint8_t>(pkt[1]) == 0x12 && pkt[2] == 0x34, "audio header");
}

void test_server() {
    std::printf("server\n");
    nb::WsServer ws;
    ws.set_hello("{\"type\":\"hello\"}");
    std::mutex bin_mu;
    std::vector<std::pair<int, std::string>> binaries;
    std::atomic<int> closed_id{0};
    ws.on_binary = [&](int client, const std::string& m) {
        std::lock_guard<std::mutex> lk(bin_mu);
        binaries.push_back({client, m});
    };
    ws.on_close = [&](int client) { closed_id = client; };
    ws.start("127.0.0.1", 0);

    {
        Client plain(ws.port(), false);
        CHECK(plain.head().find(" 400 ") != std::string::npos, "plain HTTP refused");
    }

    Client c(ws.port());
    const std::string h = c.head();
    CHECK(h.find(" 101 ") != std::string::npos && h.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos,
          "handshake: %s", h.c_str());
    uint8_t op = 0;
    std::string p;
    CHECK(c.frame(op, p) && op == 1 && p == "{\"type\":\"hello\"}", "hello first");
    CHECK(wait_for([&] { return ws.clients() == 1; }), "client counted");

    c.send_frame(0x1, "{\"cmd\":\"agc\",\"mode\":4}");
    std::string cmd;
    int from = 0;
    CHECK(wait_for([&] { return ws.next_command(cmd, from); }) && cmd == "{\"cmd\":\"agc\",\"mode\":4}",
          "command queued");
    CHECK(from > 0, "command carries the client id");
    c.send_frame(0x2, std::string("\x03\x00\x05mic", 6));
    CHECK(wait_for([&] {
              std::lock_guard<std::mutex> lk(bin_mu);
              return binaries.size() == 1;
          }),
          "binary message handed over");
    {
        std::lock_guard<std::mutex> lk(bin_mu);
        CHECK(!binaries.empty() && binaries[0].first == from && binaries[0].second.size() == 6, "binary from the same client");
    }

    c.send_frame(0x9, "hi");
    CHECK(c.frame(op, p) && op == 0xA && p == "hi", "pong");

    ws.broadcast_audio(std::string("\x01\x00\x07opus", 7));
    ws.broadcast_text("{\"type\":\"status\"}");
    CHECK(c.frame(op, p) && op == 2 && p.size() == 7, "audio frame");
    CHECK(c.frame(op, p) && op == 1 && p == "{\"type\":\"status\"}", "status frame");

    // A client that stops reading: audio is dropped, text still queued.
    ws.max_audio_backlog = 4096;
    const std::string chunk(1000, 'a');
    for (int i = 0; i < 20000 && ws.dropped_audio_frames() == 0; ++i) {
        ws.broadcast_audio(chunk);
        if (i % 50 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(ws.dropped_audio_frames() > 0, "backlog drops audio");
    ws.broadcast_text("{\"type\":\"last\"}");
    bool got_last = false;
    for (int i = 0; i < 100000 && !got_last; ++i) {
        if (!c.frame(op, p, 2000)) break;
        got_last = op == 1 && p == "{\"type\":\"last\"}";
    }
    CHECK(got_last, "text not dropped");

    c.send_frame(0x8, std::string("\x03\xe8", 2));
    CHECK(c.frame(op, p) && op == 8, "close echoed");
    CHECK(wait_for([&] { return ws.clients() == 0; }), "client gone");
    CHECK(wait_for([&] { return closed_id.load() == from; }), "on_close for the client (%d)", closed_id.load());
    ws.stop();
}

void test_opus() {
    std::printf("opus\n");
    nb::OpusAudio enc(16000);
    std::vector<float> pcm(48000);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = 0.3f * std::sin(2 * M_PI * 1000.0 * i / 48000.0);
    std::vector<std::string> packets;
    // Odd-sized pushes, like WDSP's 256-sample output blocks.
    for (size_t i = 0; i < pcm.size(); i += 256)
        enc.push(&pcm[i], std::min<size_t>(256, pcm.size() - i), [&](const std::string& p) { packets.push_back(p); });
    CHECK(packets.size() == 50, "50 packets per second, got %zu", packets.size());
    size_t bytes = 0;
    for (const auto& p : packets) bytes += p.size();
    CHECK(bytes * 8 < 20000, "about 16 kbit/s, got %zu bit/s", bytes * 8);

    int err = 0;
    OpusDecoder* dec = opus_decoder_create(48000, 1, &err);
    std::vector<float> out(960);
    double e_tone = 0, e_total = 0;
    for (size_t k = 10; k < packets.size(); ++k) {
        const int n = opus_decode_float(dec, reinterpret_cast<const unsigned char*>(packets[k].data()),
                                        static_cast<int>(packets[k].size()), out.data(), 960, 0);
        CHECK(n == 960, "decode %d", n);
        double c = 0, s = 0;
        for (int i = 0; i < n; ++i) {
            c += out[i] * std::cos(2 * M_PI * 1000.0 * i / 48000.0);
            s += out[i] * std::sin(2 * M_PI * 1000.0 * i / 48000.0);
            e_total += out[i] * out[i];
        }
        e_tone += 2.0 * (c * c + s * s) / n;
    }
    opus_decoder_destroy(dec);
    CHECK(e_tone > 0.9 * e_total, "1 kHz tone survives: %.2f", e_tone / e_total);
    std::printf("  %zu bytes/s, tone energy %.1f%%\n", bytes, 100.0 * e_tone / e_total);
}

}  // namespace

// MaiaWaterfall against a stand-in for maia-httpd's /waterfall (our own
// server sends the same kind of binary messages: 4096 float32).
void test_maia_waterfall() {
    nb::WsServer fake;
    fake.start("127.0.0.1", 0);
    nb::MaiaWaterfall wf("127.0.0.1", fake.port());
    wf.start();
    for (int i = 0; i < 100 && fake.clients() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(fake.clients() == 1, "waterfall client did not connect");
    std::vector<float> line(4096, 1e-6f);
    line[0] = 3.0f;  // maia's flag
    line[2100] = 0.25f;
    fake.broadcast_spectrum(std::string(reinterpret_cast<const char*>(line.data()), line.size() * 4));
    std::vector<float> got;
    for (int i = 0; i < 100 && !wf.take_line(got); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(got.size() == 4096, "received %zu bins", got.size());
    if (got.size() == 4096) {
        CHECK(got[2100] == 0.25f && got[5] == 1e-6f, "values changed");
        CHECK(got[0] == got[1], "flag bin not neutralised");
    }
    CHECK(wf.lines() == 1, "%llu lines", static_cast<unsigned long long>(wf.lines()));
    wf.stop();
    fake.stop();
}

int main() {
    test_helpers();
    test_server();
    test_opus();
    test_maia_waterfall();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all websocket tests passed\n");
    return 0;
}
