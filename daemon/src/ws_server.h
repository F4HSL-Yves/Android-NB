// Small WebSocket server (RFC 6455) for the phone app: one thread, poll(),
// non-blocking sockets, no external library. The DSP loop broadcasts audio
// packets and status lines; text messages from the clients are queued as
// commands for the DSP loop.
//
// Audio is real time: when a client falls behind (a slow cellular link),
// new audio frames are dropped for that client instead of piling up, so
// the delay stays short. Text frames are never dropped.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nb {

// Exposed for tests.
std::string sha1_digest(const std::string& data);  // 20 raw bytes
std::string base64_encode(const std::string& data);
std::string websocket_accept_key(const std::string& client_key);
// Server-to-client frame (FIN set, unmasked).
std::string websocket_frame(uint8_t opcode, const std::string& payload);

class WsServer {
public:
    WsServer() = default;
    ~WsServer();
    WsServer(const WsServer&) = delete;
    WsServer& operator=(const WsServer&) = delete;

    // Binds and starts the server thread. port 0 picks a free port.
    void start(const std::string& bind_address, int port);
    void stop();
    int port() const { return port_; }

    // Sent to every client right after the handshake.
    void set_hello(const std::string& text);
    void broadcast_text(const std::string& text);
    // Dropped for a client whose backlog exceeds max_audio_backlog bytes.
    void broadcast_audio(const std::string& payload);
    // Waterfall lines: dropped earlier than audio, so they never delay it.
    void broadcast_spectrum(const std::string& payload);
    // Next text message from any client; false when none is waiting.
    bool next_command(std::string& text);

    int clients() const { return clients_.load(); }
    uint64_t dropped_audio_frames() const { return dropped_.load(); }

    size_t max_audio_backlog = 24 * 1024;  // ~4 s at 48 kbit/s, ~12 s at 16
    size_t max_spectrum_backlog = 4 * 1024;
    size_t max_text_backlog = 512 * 1024;  // beyond: client is dead, close it

private:
    struct Client {
        int fd = -1;
        bool open = false;     // handshake done
        bool closing = false;  // close after flushing out
        std::string in, out, message;
        uint8_t message_opcode = 0;
    };
    struct Outgoing {
        size_t drop_above;  // backlog beyond which this frame is dropped, 0: never
        bool audio;         // counted in dropped_audio_frames()
        std::string frame;
    };

    void run();
    void accept_client();
    bool read_client(Client& c);
    bool handshake(Client& c);
    bool parse_frames(Client& c);
    bool flush(Client& c);
    void queue(Client& c, const Outgoing& o);
    void wake();

    int listen_fd_ = -1;
    int wake_fd_[2] = {-1, -1};
    int port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int> clients_{0};
    std::atomic<uint64_t> dropped_{0};

    std::mutex mu_;  // guards pending_, commands_, hello_
    std::vector<Outgoing> pending_;
    std::deque<std::string> commands_;
    std::string hello_;
    std::vector<Client> list_;  // server thread only
};

}  // namespace nb
