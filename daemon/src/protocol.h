// Messages between nbd and the phone app over the WebSocket.
//
// Server -> app
//   text   {"type":"hello","version":1,...}   once, on connection
//   text   {"type":"status",...}             every second
//   binary 0x01, seq (uint16 big endian), one Opus packet (20 ms, 48 kHz mono)
//   binary 0x02, waterfall line, all integers big endian:
//          seq u16, view centre - band centre (Hz) i32, span (Hz) u32,
//          ref (0.1 dB) i16, step (0.1 dB) u8, then one u8 per bin:
//          power dBFS = (ref + value * step) / 10, 0 also meaning "no data".
//          Frequencies are true downlink frequencies (LNB drift removed).
//
// App -> server (text, one JSON object per message)
//   {"cmd":"freq","hz":10489900000}         USB dial frequency (downlink)
//   {"cmd":"filter","low":200,"high":2800}  audio passband, Hz
//   {"cmd":"agc","mode":3}                  0 off, 1 long, 2 slow, 3 medium, 4 fast
//   {"cmd":"bitrate","bps":16000}           Opus bitrate, shared by all clients
//   {"cmd":"spectrum","center":10489750000,"span":768000,"bins":768,"rate":8}
//                                           waterfall view, shared; rate 0 = off
#pragma once

#include <cstdint>
#include <string>

namespace nb {

constexpr int kProtocolVersion = 1;
constexpr uint8_t kAudioOpus = 0x01;
constexpr uint8_t kSpectrumLine = 0x02;

struct Command {
    enum Type { None, Freq, Filter, Agc, Bitrate, Spectrum } type = None;
    double a = 0.0, b = 0.0, c = 0.0, d = 0.0;
};

// Returns a Command with type None (and fills `error`) when not understood.
Command parse_command(const std::string& json, std::string& error);

// String field of a flat JSON object ("cmd":"freq"). False when absent.
bool json_string(const std::string& json, const std::string& key, std::string& value);

struct StatusInfo {
    double freq_hz = 0;
    const char* beacon = "searching";
    double correction_hz = 0, drift_hz_per_s = 0, snr_db = 0, signal_db = 0;
    double filter_low = 0, filter_high = 0;
    int agc = 0, bitrate = 0, clients = 0;
    uint64_t dropped = 0;
    double uptime_s = 0;
};
std::string status_json(const StatusInfo& s);

// spectrum: "maia" (FPGA), "local" (FFT in nbd) or "off".
std::string hello_json(double center_hz, double rate, double min_hz, double max_hz, const std::string& source,
                       const std::string& spectrum = "off");

std::string audio_packet(uint16_t seq, const std::string& opus);

}  // namespace nb
