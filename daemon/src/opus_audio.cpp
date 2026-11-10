#include "opus_audio.h"

#include <opus/opus.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace nb {

OpusAudio::OpusAudio(int bitrate_bps, int complexity) : bitrate_(bitrate_bps) {
    int err = 0;
    enc_ = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !enc_) throw std::runtime_error(std::string("opus: ") + opus_strerror(err));
    opus_encoder_ctl(enc_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc_, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND));
    opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(std::clamp(complexity, 0, 10)));
    // Over 4G a lost packet is concealed better with in-band FEC.
    opus_encoder_ctl(enc_, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(enc_, OPUS_SET_PACKET_LOSS_PERC(5));
    set_bitrate(bitrate_bps);
    scratch_.resize(1500);
}

OpusAudio::~OpusAudio() {
    if (enc_) opus_encoder_destroy(enc_);
}

void OpusAudio::set_bitrate(int bps) {
    bitrate_ = std::clamp(bps, 6000, 64000);
    opus_encoder_ctl(enc_, OPUS_SET_BITRATE(bitrate_));
}

void OpusAudio::reset() {
    buf_.clear();
    opus_encoder_ctl(enc_, OPUS_RESET_STATE);
}

void OpusAudio::push(const float* pcm, size_t n, const std::function<void(const std::string&)>& packet) {
    // libopus asserts (abort) on some non-finite input: never hand it NaN,
    // infinity or wildly out-of-range samples.
    const size_t start = buf_.size();
    buf_.insert(buf_.end(), pcm, pcm + n);
    for (size_t i = start; i < buf_.size(); ++i) {
        float& v = buf_[i];
        if (!std::isfinite(v)) {
            v = 0.0f;
            ++bad_samples_;
        } else if (v > 1.0f) {
            v = 1.0f;
        } else if (v < -1.0f) {
            v = -1.0f;
        }
    }
    size_t pos = 0;
    while (buf_.size() - pos >= static_cast<size_t>(kFrame)) {
        const opus_int32 len = opus_encode_float(enc_, buf_.data() + pos, kFrame,
                                                 reinterpret_cast<unsigned char*>(&scratch_[0]),
                                                 static_cast<opus_int32>(scratch_.size()));
        pos += kFrame;
        if (len > 0) packet(scratch_.substr(0, static_cast<size_t>(len)));
    }
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(pos));
}

OpusMicDecoder::OpusMicDecoder() {
    int err = 0;
    dec_ = opus_decoder_create(OpusAudio::kRate, 1, &err);
    if (err != OPUS_OK || !dec_) throw std::runtime_error(std::string("opus decoder: ") + opus_strerror(err));
    scratch_.resize(5760);  // 120 ms, the longest Opus frame
}

OpusMicDecoder::~OpusMicDecoder() {
    if (dec_) opus_decoder_destroy(dec_);
}

bool OpusMicDecoder::decode(const std::string& packet, std::vector<float>& out) {
    if (packet.empty()) return false;
    const int n = opus_decode_float(dec_, reinterpret_cast<const unsigned char*>(packet.data()),
                                    static_cast<opus_int32>(packet.size()), scratch_.data(),
                                    static_cast<int>(scratch_.size()), 0);
    if (n <= 0) return false;
    out.insert(out.end(), scratch_.begin(), scratch_.begin() + n);
    return true;
}

void OpusMicDecoder::conceal(std::vector<float>& out) {
    const int n = opus_decode_float(dec_, nullptr, 0, scratch_.data(), OpusAudio::kFrame, 0);
    if (n > 0) out.insert(out.end(), scratch_.begin(), scratch_.begin() + n);
    else out.insert(out.end(), OpusAudio::kFrame, 0.0f);
}

void OpusMicDecoder::reset() { opus_decoder_ctl(dec_, OPUS_RESET_STATE); }

}  // namespace nb
