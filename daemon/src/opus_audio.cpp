#include "opus_audio.h"

#include <opus/opus.h>

#include <algorithm>
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
    buf_.insert(buf_.end(), pcm, pcm + n);
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

}  // namespace nb
