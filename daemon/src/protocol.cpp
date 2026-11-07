#include "protocol.h"

#include <cstdio>

#include "http_client.h"  // json_number

namespace nb {

bool json_string(const std::string& json, const std::string& key, std::string& value) {
    const std::string k = "\"" + key + "\"";
    size_t p = json.find(k);
    if (p == std::string::npos) return false;
    p = json.find(':', p + k.size());
    if (p == std::string::npos) return false;
    p = json.find('"', p + 1);
    if (p == std::string::npos) return false;
    const size_t e = json.find('"', p + 1);
    if (e == std::string::npos) return false;
    value = json.substr(p + 1, e - p - 1);
    return true;
}

Command parse_command(const std::string& json, std::string& error) {
    Command c;
    std::string cmd;
    if (!json_string(json, "cmd", cmd)) {
        error = "missing cmd";
        return c;
    }
    auto num = [&](const char* key, double& v) {
        if (json_number(json, key, v)) return true;
        error = cmd + ": missing " + key;
        return false;
    };
    if (cmd == "freq") {
        if (num("hz", c.a)) c.type = Command::Freq;
    } else if (cmd == "filter") {
        if (num("low", c.a) && num("high", c.b)) {
            if (c.a < 0 || c.b <= c.a + 100 || c.b > 6000) error = "filter: bad passband";
            else c.type = Command::Filter;
        }
    } else if (cmd == "agc") {
        if (num("mode", c.a)) {
            if (c.a < 0 || c.a > 4) error = "agc: mode 0..4";
            else c.type = Command::Agc;
        }
    } else if (cmd == "bitrate") {
        if (num("bps", c.a)) c.type = Command::Bitrate;
    } else {
        error = "unknown cmd " + cmd;
    }
    return c;
}

std::string status_json(const StatusInfo& s) {
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "{\"type\":\"status\",\"freq\":%.0f,\"beacon\":\"%s\",\"correction\":%.1f,\"drift\":%.3f,"
                  "\"snr\":%.1f,\"s\":%.1f,\"filter\":[%.0f,%.0f],\"agc\":%d,\"bitrate\":%d,\"clients\":%d,"
                  "\"dropped\":%llu,\"uptime\":%.0f}",
                  s.freq_hz, s.beacon, s.correction_hz, s.drift_hz_per_s, s.snr_db, s.signal_db, s.filter_low,
                  s.filter_high, s.agc, s.bitrate, s.clients, static_cast<unsigned long long>(s.dropped), s.uptime_s);
    return buf;
}

std::string hello_json(double center_hz, double rate, double min_hz, double max_hz, const std::string& source) {
    char buf[384];
    std::snprintf(buf, sizeof buf,
                  "{\"type\":\"hello\",\"version\":%d,\"center\":%.0f,\"rate\":%.0f,\"min_freq\":%.0f,"
                  "\"max_freq\":%.0f,\"audio\":{\"codec\":\"opus\",\"rate\":48000,\"channels\":1,\"frame_ms\":20},"
                  "\"source\":\"%s\"}",
                  kProtocolVersion, center_hz, rate, min_hz, max_hz, source.c_str());
    return buf;
}

std::string audio_packet(uint16_t seq, const std::string& opus) {
    std::string p;
    p.reserve(3 + opus.size());
    p.push_back(static_cast<char>(kAudioOpus));
    p.push_back(static_cast<char>(seq >> 8));
    p.push_back(static_cast<char>(seq & 0xFF));
    return p + opus;
}

}  // namespace nb
