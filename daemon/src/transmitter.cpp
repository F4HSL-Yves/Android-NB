#include "transmitter.h"

#include <cmath>
#include <cstdio>
#include <exception>
#include <vector>

#include "opus_audio.h"
#include "wdsp_tx.h"

namespace nb {

namespace {
using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o.push_back('\\');
        if (static_cast<unsigned char>(c) >= 0x20) o.push_back(c);
    }
    return o;
}
}  // namespace

std::string tx_status_json(const TxStatus& s) {
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "{\"type\":\"tx\",\"state\":\"%s\",\"message\":\"%s\",\"owner\":%d,\"uplink\":%.0f,\"lo\":%.0f,"
                  "\"cal\":%.0f,\"tune\":%s,\"mic\":%.1f,\"alc\":%.1f,\"out\":%.1f,\"underruns\":%llu,"
                  "\"dropped\":%llu,\"on_s\":%.1f,\"dac\":%.0f,\"sink\":\"%s\"}",
                  s.state.c_str(), json_escape(s.message).c_str(), s.owner, s.uplink_hz, s.lo_hz, s.cal_hz,
                  s.tune ? "true" : "false", std::fmax(-200.0, s.mic_db), s.alc_db, std::fmax(-200.0, s.out_db),
                  static_cast<unsigned long long>(s.underruns), static_cast<unsigned long long>(s.dropped), s.on_s, s.dac_sps, s.sink.c_str());
    return buf;
}

Transmitter::Transmitter(TransmitterConfig cfg, SinkFactory make_sink)
    : cfg_(cfg), make_sink_(std::move(make_sink)), jitter_(4, 15) {
    thread_ = std::thread([this] { run(); });
}

Transmitter::~Transmitter() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
        want_on_ = false;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

std::string Transmitter::request(int client, bool on) {
    std::lock_guard<std::mutex> lk(mu_);
    if (on) {
        if (want_on_ && owner_ != client) return "transmitter busy: another phone is transmitting";
        if (!want_on_) {
            jitter_.reset();
            stop_reason_.clear();
            stop_emergency_ = false;
        }
        const double up = dial_hz_ - cfg_.uplink_shift_hz + cal_hz_;
        if (up < cfg_.min_uplink_hz || up > cfg_.max_uplink_hz) return "dial outside the NB uplink";
        want_on_ = true;
        owner_ = client;
        last_mic_ = Clock::now();
        cv_.notify_all();
    } else if (want_on_ && (owner_ == client || client < 0)) {
        want_on_ = false;
        stop_reason_.clear();
        stop_emergency_ = false;
    }
    return "";
}

void Transmitter::set_dial(double downlink_hz) {
    std::lock_guard<std::mutex> lk(mu_);
    dial_hz_ = downlink_hz;
}

void Transmitter::set_cal(double hz) {
    std::lock_guard<std::mutex> lk(mu_);
    cal_hz_ = hz;
}

void Transmitter::set_tune(bool on, double audio_hz) {
    std::lock_guard<std::mutex> lk(mu_);
    tune_ = on;
    tune_hz_ = audio_hz;
    tune_dirty_ = true;
}

void Transmitter::set_mic_gain_db(double db) {
    std::lock_guard<std::mutex> lk(mu_);
    mic_gain_db_ = db;
    mic_gain_dirty_ = true;
}

void Transmitter::set_passband(double low_hz, double high_hz) {
    std::lock_guard<std::mutex> lk(mu_);
    pass_low_ = low_hz;
    pass_high_ = high_hz;
    pass_dirty_ = true;
}

void Transmitter::mic_packet(int client, uint16_t seq, std::string opus) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!want_on_ || client != owner_) return;
        last_mic_ = Clock::now();
    }
    jitter_.push(seq, std::move(opus));
}

void Transmitter::client_closed(int client) {
    std::lock_guard<std::mutex> lk(mu_);
    if (want_on_ && owner_ == client) {
        want_on_ = false;
        stop_reason_ = "phone disconnected";
        stop_emergency_ = true;
    }
}

TxStatus Transmitter::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    return st_;
}

void Transmitter::notify() {
    TxStatus s;
    {
        std::lock_guard<std::mutex> lk(mu_);
        s = st_;
    }
    if (on_status) on_status(s);
}

void Transmitter::finish(const std::string& state, const std::string& message, bool emergency) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        want_on_ = false;
        owner_ = -1;
        st_.state = state;
        st_.message = message;
        st_.owner = -1;
    }
    if (emergency && on_emergency) on_emergency(message);
    notify();
}

void Transmitter::run() {
    // The WDSP channel is opened once, here: its FFT plans take a moment.
    std::unique_ptr<WdspTx> wdsp;
    try {
        wdsp = std::make_unique<WdspTx>(cfg_.wdsp_channel, cfg_.block, cfg_.dsp_size);
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(mu_);
        st_.state = "error";
        st_.message = e.what();
        return;
    }
    OpusMicDecoder decoder;
    while (true) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stop_ || want_on_; });
            if (stop_) return;
            st_ = TxStatus();
            st_.state = "starting";
            st_.owner = owner_;
            st_.cal_hz = cal_hz_;
            tune_dirty_ = mic_gain_dirty_ = pass_dirty_ = true;
        }
        notify();

        std::unique_ptr<TxSink> sink;
        try {
            sink = make_sink_();
            sink->open();
        } catch (const std::exception& e) {
            if (sink) sink->mute();
            finish("error", e.what(), false);
            continue;
        }

        std::vector<float> pcm;
        std::vector<cf> base;
        std::vector<int16_t> iq;
        decoder.reset();
        wdsp->flush();
        std::unique_ptr<Upconverter> up;
        try {
            up = std::make_unique<Upconverter>(sink->rate());
        } catch (const std::exception& e) {
            sink->mute();
            sink->close();
            finish("error", e.what(), false);
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            st_.state = "on";
            st_.lo_hz = sink->lo_hz();
            st_.sink = sink->kind();
        }
        notify();

        const auto started = Clock::now();
        auto next_meter = started;
        std::string end_state = "off", end_message;
        bool emergency = false;
        uint64_t written = 0, pace_w = 0;
        double pace_t = -1;
        try {
            while (true) {
                double dial, cal;
                {
                    std::unique_lock<std::mutex> lk(mu_);
                    if (stop_ || !want_on_) {
                        end_message = stop_reason_;
                        emergency = stop_emergency_;
                        break;
                    }
                    if (seconds_since(last_mic_) > cfg_.mic_timeout_s) {
                        end_message = "microphone stream lost";
                        emergency = true;
                        break;
                    }
                    if (tune_dirty_) wdsp->set_tune(tune_, tune_hz_);
                    if (mic_gain_dirty_) wdsp->set_mic_gain_db(mic_gain_db_);
                    if (pass_dirty_) wdsp->set_passband(pass_low_, pass_high_);
                    tune_dirty_ = mic_gain_dirty_ = pass_dirty_ = false;
                    dial = dial_hz_;
                    cal = cal_hz_;
                }
                if (seconds_since(started) > cfg_.max_tx_s) {
                    end_message = "maximum transmit time reached";
                    emergency = true;
                    break;
                }
                const double uplink = dial - cfg_.uplink_shift_hz + cal;
                if (uplink < cfg_.min_uplink_hz || uplink > cfg_.max_uplink_hz) {
                    end_message = "dial outside the NB uplink";
                    emergency = true;
                    break;
                }

                // Microphone: decoded packets, concealment for a lost one,
                // silence when the buffer runs dry.
                while (pcm.size() < static_cast<size_t>(cfg_.block)) {
                    std::string packet;
                    const auto r = jitter_.pop(packet);
                    if (r == PacketJitter::Pop::Empty) break;
                    if (r == PacketJitter::Pop::Packet) {
                        if (!decoder.decode(packet, pcm)) decoder.conceal(pcm);
                    } else {
                        decoder.conceal(pcm);
                    }
                }
                if (pcm.size() < static_cast<size_t>(cfg_.block)) pcm.resize(static_cast<size_t>(cfg_.block), 0.0f);
                wdsp->process(pcm.data(), base);
                pcm.erase(pcm.begin(), pcm.begin() + cfg_.block);

                up->set_offset_hz(uplink - sink->lo_hz());
                iq.clear();
                up->process(base.data(), base.size(), iq);
                sink->write(iq.data(), iq.size() / 2);
                written += iq.size() / 2;

                // The DAC paces this loop: a wrong pace means the FPGA is not
                // at the rate the IQ were made for (its x8 interpolator left
                // on puts the signal at offset/8, outside the band). Checked
                // over each second, after the first half second (the
                // kernel blocks fill without waiting).
                {
                    const double el = seconds_since(started);
                    if (pace_t < 0) {
                        if (el >= 0.5) pace_t = el, pace_w = written;
                    } else if (el - pace_t >= 1.0) {
                        const double sps = static_cast<double>(written - pace_w) / (el - pace_t);
                        if (std::fabs(sps - sink->rate()) > 0.1 * sink->rate()) {
                            char m[160];
                            std::snprintf(m, sizeof m, "the DAC takes %.0f S/s instead of %.0f: transmission stopped",
                                          sps, sink->rate());
                            end_state = "error";
                            end_message = m;
                            emergency = true;
                            break;
                        }
                        pace_t = el, pace_w = written;
                    }
                }

                const auto now = Clock::now();
                if (now >= next_meter) {
                    next_meter = now + std::chrono::milliseconds(200);
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        st_.uplink_hz = uplink;
                        st_.cal_hz = cal;
                        st_.tune = wdsp->tune();
                        st_.mic_db = wdsp->mic_peak_db();
                        st_.alc_db = wdsp->alc_gain_db();
                        st_.out_db = wdsp->out_peak_db();
                        st_.underruns = jitter_.underruns();
                        st_.dropped = jitter_.dropped();
                        st_.on_s = seconds_since(started);
                        if (st_.on_s > 0) st_.dac_sps = static_cast<double>(written) / st_.on_s;
                    }
                    notify();
                }
            }
        } catch (const std::exception& e) {
            end_state = "error";
            end_message = e.what();
            emergency = true;
        }
        sink->mute();
        try {
            sink->close();
        } catch (const std::exception&) {
        }
        finish(end_state, end_message, emergency);
    }
}

}  // namespace nb
