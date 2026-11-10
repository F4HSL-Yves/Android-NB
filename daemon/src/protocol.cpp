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
    } else if (cmd == "spectrum") {
        if (num("center", c.a) && num("span", c.b) && num("bins", c.c) && num("rate", c.d)) {
            if (c.b < 0 || c.c < 1 || c.d < 0) error = "spectrum: bad view";
            else c.type = Command::Spectrum;
        }
    } else if (cmd == "bitrate") {
        if (num("bps", c.a)) c.type = Command::Bitrate;
    } else if (cmd == "tx") {
        if (num("on", c.a)) c.type = Command::Tx;
    } else if (cmd == "tune") {
        if (num("on", c.a)) {
            if (!json_number(json, "hz", c.b)) c.b = 1000.0;
            if (c.b < 100 || c.b > 3000) error = "tune: hz 100..3000";
            else c.type = Command::Tune;
        }
    } else if (cmd == "txcal") {
        if (num("hz", c.a)) {
            if (c.a < -50000 || c.a > 50000) error = "txcal: hz -50000..50000";
            else c.type = Command::TxCal;
        }
    } else if (cmd == "micgain") {
        if (num("db", c.a)) {
            if (c.a < -20 || c.a > 30) error = "micgain: db -20..30";
            else c.type = Command::MicGain;
        }
    } else if (cmd == "nr") {
        if (num("mode", c.a)) {
            if (c.a < 0 || c.a > 2) error = "nr: mode 0..2";
            else c.type = Command::Nr;
        }
    } else if (cmd == "txcal_auto") {
        c.type = Command::TxCalAuto;
    } else if (cmd == "comp") {
        if (num("on", c.a)) {
            if (!json_number(json, "db", c.b)) c.b = 10.0;
            if (c.b < 0 || c.b > 20) error = "comp: db 0..20";
            else c.type = Command::Comp;
        }
    } else {
        error = "unknown cmd " + cmd;
    }
    return c;
}

std::string status_json(const StatusInfo& s) {
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "{\"type\":\"status\",\"freq\":%.0f,\"beacon\":\"%s\",\"correction\":%.1f,\"drift\":%.3f,"
                  "\"snr\":%.1f,\"s\":%.1f,\"filter\":[%.0f,%.0f],\"agc\":%d,\"nr\":%d,\"bitrate\":%d,\"clients\":%d,"
                  "\"dropped\":%llu,\"uptime\":%.0f}",
                  s.freq_hz, s.beacon, s.correction_hz, s.drift_hz_per_s, s.snr_db, s.signal_db, s.filter_low,
                  s.filter_high, s.agc, s.nr, s.bitrate, s.clients, static_cast<unsigned long long>(s.dropped), s.uptime_s);
    return buf;
}

std::string hello_json(double center_hz, double rate, double min_hz, double max_hz, const std::string& source,
                       const std::string& spectrum, bool tx) {
    char buf[480];
    std::snprintf(buf, sizeof buf,
                  "{\"type\":\"hello\",\"version\":%d,\"center\":%.0f,\"rate\":%.0f,\"min_freq\":%.0f,"
                  "\"max_freq\":%.0f,\"audio\":{\"codec\":\"opus\",\"rate\":48000,\"channels\":1,\"frame_ms\":20},"
                  "\"source\":\"%s\",\"spectrum\":{\"source\":\"%s\",\"max_bins\":2048},\"tx\":%s}",
                  kProtocolVersion, center_hz, rate, min_hz, max_hz, source.c_str(), spectrum.c_str(),
                  tx ? "true" : "false");
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

bool parse_mic_packet(const std::string& m, uint16_t& seq, std::string& opus) {
    if (m.size() < 4 || static_cast<uint8_t>(m[0]) != kMicOpus) return false;
    seq = static_cast<uint16_t>((static_cast<uint8_t>(m[1]) << 8) | static_cast<uint8_t>(m[2]));
    opus = m.substr(3);
    return true;
}

}  // namespace nb
