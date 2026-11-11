// PlutoSource against a fake board: a small iiod + maia-httpd server on
// localhost that records every setting and streams a synthetic beacon. Checks
// the frequency plan, the DDC request, the DMA route (set, then released) and
// that the beacon lock runs on the live stream.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "beacon_lock.h"
#include "http_client.h"
#include "iiod_client.h"
#include "pluto_source.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
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

// ---------------------------------------------------------------- helpers

int listen_any(int& port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd, 4) != 0) {
        std::perror("bind");
        std::exit(1);
    }
    socklen_t len = sizeof a;
    getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    port = ntohs(a.sin_port);
    return fd;
}

bool read_line(int fd, std::string& line) {
    line.clear();
    char c;
    while (true) {
        const ssize_t r = ::recv(fd, &c, 1, 0);
        if (r <= 0) return false;
        if (c == '\n') return true;
        if (c != '\r') line.push_back(c);
    }
}

bool read_n(int fd, std::string& out, size_t n) {
    out.assign(n, '\0');
    size_t got = 0;
    while (got < n) {
        const ssize_t r = ::recv(fd, &out[got], n - got, 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

void send_all(int fd, const std::string& s) {
    size_t sent = 0;
    while (sent < s.size()) {
        const ssize_t w = ::send(fd, s.data() + sent, s.size() - sent, MSG_NOSIGNAL);
        if (w <= 0) return;
        sent += static_cast<size_t>(w);
    }
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> v;
    size_t p = 0;
    while (p < s.size()) {
        const size_t q = s.find(' ', p);
        v.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return v;
}

// ---------------------------------------------------------------- fake board

const char* kContextXml =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?><context name=\"network\">"
    "<device id=\"iio:device0\" name=\"ad9361-phy\">"
    "<channel id=\"voltage0\" type=\"input\"><attribute name=\"sampling_frequency\" /></channel>"
    "<channel id=\"altvoltage1\" name=\"TX_LO\" type=\"output\"><attribute name=\"frequency\" /></channel>"
    "<channel id=\"altvoltage0\" name=\"RX_LO\" type=\"output\"><attribute name=\"frequency\" /></channel>"
    "<channel id=\"voltage0\" type=\"output\"><attribute name=\"hardwaregain\" /></channel>"
    "</device>"
    "<device id=\"iio:device3\" name=\"cf-ad9361-lpc\">"
    "<channel id=\"voltage0\" type=\"input\"><scan-element index=\"0\" format=\"le:S12/16&gt;&gt;0\" /></channel>"
    "<channel id=\"voltage1\" type=\"input\"><scan-element index=\"1\" format=\"le:S12/16&gt;&gt;0\" /></channel>"
    "<channel id=\"voltage2\" type=\"input\"><scan-element index=\"2\" format=\"le:S12/16&gt;&gt;0\" /></channel>"
    "<channel id=\"voltage3\" type=\"input\"><scan-element index=\"3\" format=\"le:S12/16&gt;&gt;0\" /></channel>"
    "</device></context>";

struct FakeBoard {
    // What the client set, keyed "dev/ch/attr".
    std::mutex mu;
    std::map<std::string, std::string> attrs;
    std::map<uint32_t, uint32_t> regs;
    uint32_t reg_addr = 0;
    std::vector<int> route_history;  // bit 0 of 0x800000BC after each write
    std::string opened_mask;
    size_t opened_samples = 0;
    bool closed = false;
    std::vector<std::string> http_log;  // "METHOD path body"
    std::string refuse_rate;            // sampling_frequency the AD9363 "rounds" to

    // Synthetic downlink at the DDC output: beacon offset by an LNB error.
    double fs = 768000.0;
    double beacon_hz = -250000.0 + 1800.0;
    uint64_t n = 0;
    std::mt19937 rng{7};
    std::normal_distribution<double> noise{0.0, 40.0};

    int iiod_fd = -1, http_fd = -1;
    int iiod_port = 0, http_port = 0;
    std::atomic<bool> stop{false};
    std::thread iiod_thread, http_thread;

    FakeBoard() {
        iiod_fd = listen_any(iiod_port);
        http_fd = listen_any(http_port);
        regs[0x800000BC] = 0x10;  // other bits must survive
        iiod_thread = std::thread([this] { serve_iiod(); });
        http_thread = std::thread([this] { serve_http(); });
    }
    ~FakeBoard() {
        stop = true;
        ::shutdown(iiod_fd, SHUT_RDWR);
        ::shutdown(http_fd, SHUT_RDWR);
        ::close(iiod_fd);
        ::close(http_fd);
        iiod_thread.join();
        http_thread.join();
    }

    std::string samples(size_t bytes) {
        std::string out(bytes, '\0');
        for (size_t i = 0; i + 4 <= bytes; i += 4, ++n) {
            const double ph = 2.0 * kPi * beacon_hz * static_cast<double>(n) / fs;
            const auto i16 = static_cast<int16_t>(std::lrint(400.0 * std::cos(ph) + noise(rng)));
            const auto q16 = static_cast<int16_t>(std::lrint(400.0 * std::sin(ph) + noise(rng)));
            std::memcpy(&out[i], &i16, 2);
            std::memcpy(&out[i + 2], &q16, 2);
        }
        return out;
    }

    void serve_iiod() {
        while (!stop) {
            const int c = ::accept(iiod_fd, nullptr, nullptr);
            if (c < 0) return;
            std::string line, data;
            while (read_line(c, line)) {
                const auto t = split(line);
                if (t.empty()) continue;
                std::lock_guard<std::mutex> lk(mu);
                if (t[0] == "VERSION") {
                    send_all(c, "0.26.0000000\n");
                } else if (t[0] == "PRINT") {
                    const std::string xml = kContextXml;
                    send_all(c, std::to_string(xml.size()) + "\n" + xml + "\n");
                } else if (t[0] == "WRITE" && t.size() == 5 && t[2] == "DEBUG") {
                    read_n(c, data, std::stoul(t[4]));
                    const auto v = split(data);
                    reg_addr = static_cast<uint32_t>(std::stoul(v[0], nullptr, 0));
                    if (v.size() > 1) {
                        regs[reg_addr] = static_cast<uint32_t>(std::stoul(v[1], nullptr, 0));
                        if (reg_addr == 0x800000BC) route_history.push_back(regs[reg_addr] & 1);
                    }
                    send_all(c, std::to_string(data.size()) + "\n");
                } else if (t[0] == "READ" && t.size() == 4 && t[2] == "DEBUG") {
                    char buf[32];
                    std::snprintf(buf, sizeof buf, "0x%X", regs[reg_addr]);
                    send_all(c, std::to_string(std::strlen(buf)) + "\n" + buf + "\n");
                } else if (t[0] == "WRITE" && t.size() == 6) {
                    read_n(c, data, std::stoul(t[5]));
                    std::string v = data;
                    if (t[4] == "sampling_frequency" && !refuse_rate.empty()) v = refuse_rate;
                    attrs[t[1] + "/" + t[2] + "/" + t[3] + "/" + t[4]] = v;
                    send_all(c, std::to_string(data.size()) + "\n");
                } else if (t[0] == "READ" && t.size() == 5) {
                    const std::string v = attrs[t[1] + "/" + t[2] + "/" + t[3] + "/" + t[4]];
                    send_all(c, std::to_string(v.size()) + "\n" + v + "\n");
                } else if (t[0] == "SET") {
                    send_all(c, "0\n");
                } else if (t[0] == "OPEN" && t.size() == 4) {
                    opened_samples = std::stoul(t[2]);
                    opened_mask = t[3];
                    send_all(c, "0\n");
                } else if (t[0] == "READBUF" && t.size() == 3) {
                    // Two chunks, like iiod does for large buffers.
                    const size_t total = std::stoul(t[2]);
                    const size_t first = total / 2;
                    send_all(c, std::to_string(first) + "\n" + opened_mask + "\n" + samples(first));
                    send_all(c, std::to_string(total - first) + "\n" + samples(total - first));
                } else if (t[0] == "CLOSE") {
                    closed = true;
                    send_all(c, "0\n");
                } else {
                    send_all(c, "-22\n");
                }
            }
            ::close(c);
        }
    }

    void serve_http() {
        while (!stop) {
            const int c = ::accept(http_fd, nullptr, nullptr);
            if (c < 0) return;
            std::string line, method, path, body;
            size_t len = 0;
            if (read_line(c, line)) {
                const auto t = split(line);
                method = t.size() > 0 ? t[0] : "";
                path = t.size() > 1 ? t[1] : "";
                while (read_line(c, line) && !line.empty())
                    if (line.compare(0, 15, "Content-Length:") == 0) len = std::stoul(line.substr(15));
                read_n(c, body, len);
            }
            std::string reply;
            {
                std::lock_guard<std::mutex> lk(mu);
                http_log.push_back(method + " " + path + " " + body);
            }
            if (method == "PUT" && path == "/api/ddc/design") {
                reply = "{\"frequency\":-500000.0,\"decimation\":4}";
            } else if (method == "GET" && path == "/api/ddc/config") {
                // Chunked on purpose: maia-httpd (axum) may answer that way.
                const std::string j =
                    "{\"enabled\":true,\"frequency\":-500000.0,\"decimation\":4,\"input_sampling_frequency\":3072000.0,"
                    "\"output_sampling_frequency\":768000.0}";
                char hex[16];
                std::snprintf(hex, sizeof hex, "%zx", j.size());
                send_all(c, std::string("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                        "Transfer-Encoding: chunked\r\n\r\n") +
                                hex + "\r\n" + j + "\r\n0\r\n\r\n");
                ::close(c);
                continue;
            } else if (method == "PATCH" && path == "/api/spectrometer") {
                reply = "{\"input\":\"DDC\"}";
            } else {
                send_all(c, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
                ::close(c);
                continue;
            }
            send_all(c, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                            std::to_string(reply.size()) + "\r\n\r\n" + reply);
            ::close(c);
        }
    }
};

// ---------------------------------------------------------------- tests

void test_pure_helpers() {
    std::printf("helpers\n");
    CHECK(nb::iio_channel_mask(4, {0, 1}) == "00000003", "mask 4ch");
    CHECK(nb::iio_channel_mask(40, {0, 33}) == "0000000200000001", "mask 40ch: %s",
          nb::iio_channel_mask(40, {0, 33}).c_str());

    const auto devs = nb::parse_iio_context(kContextXml);
    CHECK(devs.size() == 2, "devices %zu", devs.size());
    if (devs.size() == 2) {
        CHECK(devs[1].name == "cf-ad9361-lpc" && devs[1].id == "iio:device3", "adc name/id");
        const auto* lo = devs[0].channel("altvoltage0", true);
        CHECK(lo && lo->index_in_device == 2, "RX LO channel");
        CHECK(devs[0].channel("voltage0", false) != devs[0].channel("voltage0", true), "in/out voltage0 distinct");
        const auto* q = devs[1].channel("voltage1", false);
        CHECK(q && q->index_in_device == 1 && q->scan_element, "Q scan element");
    }

    nb::PlutoConfig cfg;
    const auto plan = nb::make_plan(cfg);
    CHECK(plan.rx_lo_hz == 740250000, "LO %lld", static_cast<long long>(plan.rx_lo_hz));
    CHECK(plan.ddc_frequency_hz == -500000.0, "DDC %.1f", plan.ddc_frequency_hz);
    CHECK(plan.output_rate == 768000.0, "rate %.1f", plan.output_rate);
    const std::string j = nb::ddc_design_json(cfg);
    double v = 0;
    CHECK(nb::json_number(j, "frequency", v) && v == -500000.0, "json frequency");
    CHECK(nb::json_number(j, "decimation", v) && v == 4.0, "json decimation");
}

void test_live_stream() {
    std::printf("live stream from a fake board\n");
    FakeBoard board;
    nb::PlutoConfig cfg;
    cfg.iiod_port = board.iiod_port;
    cfg.http_port = board.http_port;
    {
        nb::PlutoSource src(cfg);
        src.start();
        {
            std::lock_guard<std::mutex> lk(board.mu);
            CHECK(board.attrs["iio:device0/OUTPUT/altvoltage0/frequency"] == "740250000", "RX LO %s",
                  board.attrs["iio:device0/OUTPUT/altvoltage0/frequency"].c_str());
            CHECK(board.attrs["iio:device0/INPUT/voltage0/sampling_frequency"] == "3072000", "rate");
            CHECK(board.attrs["iio:device0/INPUT/voltage0/gain_control_mode"] == "slow_attack", "gain mode");
            CHECK(board.regs[0x800000BC] == 0x11, "route reg 0x%X", board.regs[0x800000BC]);
            CHECK(board.opened_mask == "00000003" && board.opened_samples == 16384, "OPEN %zu %s",
                  board.opened_samples, board.opened_mask.c_str());
            CHECK(!board.http_log.empty() && board.http_log[0].find("\"frequency\":-500000.0") != std::string::npos &&
                      board.http_log[0].find("\"decimation\":4") != std::string::npos,
                  "DDC design request");
        }
        CHECK(src.output_rate() == 768000.0, "output rate %.1f", src.output_rate());
        std::printf("  %s\n", src.summary().c_str());

        nb::BeaconLockConfig lc;
        lc.sample_rate = src.output_rate();
        nb::BeaconLock lock(lc);
        std::vector<nb::cf> iq;
        const size_t seconds = 6;
        for (size_t got = 0; got < seconds * 768000; got += iq.size()) {
            src.read(iq);
            lock.process(iq.data(), iq.size());
        }
        const auto st = lock.status();
        CHECK(st.state == nb::LockState::Locked, "lock state %s", nb::to_string(st.state));
        CHECK(std::fabs(st.correction_hz - 1800.0) < 2.0, "correction %.2f Hz", st.correction_hz);
        std::printf("  beacon %s, correction %+.2f Hz\n", nb::to_string(st.state), st.correction_hz);

        src.stop();
        src.stop();  // repeat is harmless
    }
    std::lock_guard<std::mutex> lk(board.mu);
    CHECK(board.closed, "buffer closed");
    CHECK(board.regs[0x800000BC] == 0x10, "route released, other bits kept: 0x%X", board.regs[0x800000BC]);
    CHECK(board.route_history.size() == 2 && board.route_history[0] == 1 && board.route_history[1] == 0,
          "route set then released (%zu writes)", board.route_history.size());
}

void test_refused_rate() {
    std::printf("AD9363 refuses the sample rate\n");
    FakeBoard board;
    board.refuse_rate = "2083333";
    nb::PlutoConfig cfg;
    cfg.iiod_port = board.iiod_port;
    cfg.http_port = board.http_port;
    nb::PlutoSource src(cfg);
    bool threw = false;
    try {
        src.start();
    } catch (const nb::IiodError& e) {
        threw = std::string(e.what()).find("refused 3072000") != std::string::npos;
    }
    CHECK(threw, "start must fail with a clear message");
    std::lock_guard<std::mutex> lk(board.mu);
    CHECK(board.route_history.empty(), "route untouched");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--serve") {
        // Manual end-to-end check: nbd --source pluto --iiod-port A --http-port B
        FakeBoard board;
        std::printf("--iiod-port %d --http-port %d\n", board.iiod_port, board.http_port);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(argc > 2 ? std::atoi(argv[2]) : 60));
        return 0;
    }
    test_pure_helpers();
    test_live_stream();
    test_refused_rate();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all pluto tests passed\n");
    return 0;
}
