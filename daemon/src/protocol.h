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
//   {"cmd":"rit","hz":0}                    receive offset from the dial, -10000..10000 Hz
//                                           (the uplink stays on the dial)
//   {"cmd":"filter","low":200,"high":2800}  audio passband, Hz
//   {"cmd":"agc","mode":3}                  0 off, 1 long, 2 slow, 3 medium, 4 fast
//   {"cmd":"nr","mode":2}                   noise reduction: 0 off, 1 ANR, 2 EMNR
//   {"cmd":"bitrate","bps":16000}           Opus bitrate, shared by all clients
//   {"cmd":"spectrum","center":10489750000,"span":768000,"bins":768,"rate":8}
//                                           waterfall view, shared; rate 0 = off
//   {"cmd":"tx","on":1}                     open (1) or close (0) the transmitter;
//                                           nbd runs with --tx. The RF itself is
//                                           switched over MQTT (Arduino PTT, tx/mute).
//   {"cmd":"tune","on":1,"hz":1000}         carrier at dial + hz instead of the microphone
//   {"cmd":"txcal","hz":-120}               uplink correction, Hz (board reference error)
//   {"cmd":"micgain","db":0}                microphone gain before the leveler, -20..+30 dB
//   {"cmd":"comp","on":1,"db":10}           WDSP speech compressor, 0..20 dB
//   {"cmd":"txeq","bass":0,"treble":0}      TX voice EQ, dB at 200 Hz / 2.7 kHz, -12..+12
//   {"cmd":"rxgain","db":40}                AD9363 receive gain, manual, 0..70 dB
//   {"cmd":"rxgain","auto":1}               AD9363 AGC (slow_attack)
//   {"cmd":"beacon","hz":10489750000}       beacon to lock on: 10489500000 (CW),
//                                           10489750000 (BPSK) or 10490000000 (CW)
//   {"cmd":"txcal_auto"}                    measure the own tune carrier, set the TX correction
//                                           (answer: {"type":"txcal","hz":..,"measured":..,"snr":..})
//   binary 0x03, seq (uint16 big endian), one Opus packet: microphone,
//          48 kHz mono, 20 ms. Must keep flowing while transmitting: nbd
//          stops the transmission after 1 s without one.
//
// Server -> app, transmitter
//   text   {"type":"tx","state":"off|starting|on|error","message":"...",
//           "uplink":2400250000,"lo":2399500000,"cal":0,"tune":false,
//           "mic":-12.0,"alc":0.0,"out":-3.0,"underruns":0,"dropped":0,"on_s":3.2}
//          on each change and every 200 ms while on.
#pragma once

#include <cstdint>
#include <string>

namespace nb {

constexpr int kProtocolVersion = 1;
constexpr uint8_t kAudioOpus = 0x01;
constexpr uint8_t kSpectrumLine = 0x02;
constexpr uint8_t kMicOpus = 0x03;

struct Command {
    enum Type { None, Freq, Filter, Agc, Bitrate, Spectrum, Tx, Tune, TxCal, MicGain, Comp, TxCalAuto, Nr, TxEq, Rit, RxGain, Beacon } type = None;
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
    int nr = 0;
    double beacon_hz = 0;   // beacon followed
    double rx_gain_db = -1; // manual AD9363 gain, -1 for its AGC
    uint64_t dropped = 0;
    double uptime_s = 0;
};
std::string status_json(const StatusInfo& s);

// spectrum: "maia" (FPGA), "local" (FFT in nbd) or "off".
std::string hello_json(double center_hz, double rate, double min_hz, double max_hz, const std::string& source,
                       const std::string& spectrum = "off", bool tx = false);

std::string audio_packet(uint16_t seq, const std::string& opus);

// Binary message from the app: true for a microphone packet (fills seq and opus).
bool parse_mic_packet(const std::string& message, uint16_t& seq, std::string& opus);

}  // namespace nb
