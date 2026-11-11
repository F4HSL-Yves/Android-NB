// Opus encoder for the demodulated audio: 48 kHz mono, 20 ms packets.
// SSB fits in Opus wideband; 12 to 16 kbit/s is plenty for a phone on 4G.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct OpusEncoder;
struct OpusDecoder;

namespace nb {

class OpusAudio {
public:
    static constexpr int kRate = 48000;
    static constexpr int kFrame = 960;  // 20 ms

    explicit OpusAudio(int bitrate_bps = 16000, int complexity = 5);
    ~OpusAudio();
    OpusAudio(const OpusAudio&) = delete;
    OpusAudio& operator=(const OpusAudio&) = delete;

    void set_bitrate(int bps);  // clamped to 6000..64000
    int bitrate() const { return bitrate_; }

    // Buffers samples and calls `packet` for each complete 20 ms packet.
    void push(const float* pcm, size_t n, const std::function<void(const std::string&)>& packet);
    // Drops buffered samples (no client listening).
    void reset();
    // Non-finite samples replaced by silence since start (should stay 0).
    uint64_t bad_samples() const { return bad_samples_; }

private:
    OpusEncoder* enc_ = nullptr;
    int bitrate_;
    std::vector<float> buf_;
    std::string scratch_;
    uint64_t bad_samples_ = 0;
};

// Decoder for the microphone packets from the phone (48 kHz mono, any frame
// length Opus allows). A lost packet is concealed by libopus.
class OpusMicDecoder {
public:
    OpusMicDecoder();
    ~OpusMicDecoder();
    OpusMicDecoder(const OpusMicDecoder&) = delete;
    OpusMicDecoder& operator=(const OpusMicDecoder&) = delete;

    // Appends the decoded samples; false (nothing appended) for a bad packet.
    bool decode(const std::string& packet, std::vector<float>& out);
    // Concealment for one missing 20 ms packet.
    void conceal(std::vector<float>& out);
    void reset();

private:
    OpusDecoder* dec_ = nullptr;
    std::vector<float> scratch_;
};

}  // namespace nb
