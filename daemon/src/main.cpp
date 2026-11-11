// nbd: QO-100 narrowband receiver core.
//
// First version: reads the IQ stream of the narrowband transponder (from a
// file, or from stdin so it can sit behind iio_readdev on the board), locks
// on the CW beacon, demodulates one USB station with WDSP and writes 48 kHz
// mono audio. The network side (Opus, WebSocket, MQTT) comes next.
//
// Live on the board (Tezuka + Maia): nbd sets the AD9363 and the Maia DDC
// itself, routes the DDC to the DMA and reads the IQ through iiod:
//   nbd --source pluto --freq 10489900000 --output - | aplay ...
// From a PC or phone on the same network: --source pluto:192.168.2.1
//
// Phone app: --ws 8073 serves Opus audio, status and commands over a
// WebSocket (see protocol.h), plus a waterfall of the band taken from
// Maia's FPGA spectrometer (or computed here for other sources).
//
// Transmit (--tx): the app sends its microphone in Opus; nbd modulates USB
// with WDSP and streams it to the AD9363 DAC, full duplex with reception
// (see transmitter.h). --tx-file writes the IQ to a file instead.
//
// Offline, from a recording or stdin (768 kS/s around 10489.750 MHz):
//   nbd --input capture.cs16 --freq 10489900000 --output audio.s16
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <memory>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>
#include <vector>

#include "beacon_lock.h"
#include "maia_waterfall.h"
#include "opus_audio.h"
#include "pluto_source.h"
#include "protocol.h"
#include "spectrum.h"
#include "transmitter.h"
#include "ws_server.h"
#include "tx_dsp.h"
#include "wdsp_rx.h"

namespace {

struct Options {
    std::string input = "-";
    std::string output;
    std::string format = "cs16";  // cs16 (12-bit samples in int16) or cf32
    double rate = 768000.0;
    double center_hz = 10489750000.0;  // downlink frequency at the stream centre
    double freq_hz = 10489900000.0;    // station to demodulate (USB dial)
    double beacon_hz = 10489500000.0;  // CW beacon
    double search_hz = 25000.0;
    std::string wisdom_dir;
    bool quiet = false;
    // Live source
    std::string source;  // empty: --input; "pluto" or "pluto:HOST"
    nb::PlutoConfig pluto;
    double duration_s = 0.0;  // 0: until stopped
    // Phone app
    int ws_port = 0;  // 0: no WebSocket server
    std::string ws_bind = "0.0.0.0";
    int bitrate = 16000;
    int opus_complexity = 5;
    bool realtime = false;  // pace a file input like the live stream
    bool loop = false;      // restart a file input at its end
    int dsp_size = 2048;    // WDSP block at 48 kHz
    std::string spectrum = "auto";  // waterfall: auto, maia, local, off
    // Transmitter
    bool tx = false;
    double tx_lo_hz = 2399500000.0;
    std::string tx_file;   // offline: IQ to a file instead of the DAC
    double tx_max_s = 600.0;
    std::string mqtt_host;  // Arduino PTT off when nbd stops a transmission itself
};

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

void usage() {
    std::fprintf(stderr,
                 "usage: nbd [options]\n"
                 "  --source pluto[:HOST]  live IQ from a Tezuka/Maia board (default host 127.0.0.1)\n"
                 "  --iiod-port N    iiod TCP port (default 30431)\n"
                 "  --http-port N    maia-httpd port (default 80)\n"
                 "  --lnb-lo HZ      nominal LNB local oscillator (default 9750000000)\n"
                 "  --lo-offset HZ   AD9363 LO above the centre, keeps DC out (default 500000)\n"
                 "  --gain DB|MODE   manual gain in dB, or slow_attack/fast_attack/hybrid (default slow_attack)\n"
                 "  --duration S     stop after S seconds of IQ (default: until Ctrl-C)\n"
                 "  --ws PORT        WebSocket server for the phone app (Opus audio, status, commands)\n"
                 "  --ws-bind ADDR   address to listen on (default 0.0.0.0)\n"
                 "  --bitrate BPS    Opus bitrate (default 16000)\n"
                 "  --opus-complexity N  0..10 (default 5, lower saves CPU on the board)\n"
                 "  --spectrum S     waterfall source: auto (default: maia when live, else local), maia, local, off\n"
                 "  --tx             SSB transmitter for the app (microphone over the WebSocket)\n"
                 "  --tx-lo HZ       AD9363 TX LO, below the uplink (default 2399500000)\n"
                 "  --tx-file FILE   write the transmitted IQ (cs16, 3.072 MS/s) to FILE instead of the DAC\n"
                 "  --tx-max S       longest transmission (default 600)\n"
                 "  --mqtt HOST      broker for arduino/ptt off when nbd cuts a transmission (default: the board)\n"
                 "  --dsp-size N     WDSP block at 48 kHz, 1024..8192 (default 2048; smaller = less delay)\n"
                 "  --realtime       read a file input at the real sample rate\n"
                 "  --loop           restart a file input at its end\n"
                 "  --input FILE     IQ input, '-' for stdin (default -)\n"
                 "  --format F       cs16 (default, 12-bit in int16) or cf32\n"
                 "  --rate HZ        IQ sample rate (default 768000)\n"
                 "  --center HZ      downlink frequency at the stream centre (default 10489750000)\n"
                 "  --freq HZ        USB dial frequency to demodulate (default 10489900000)\n"
                 "  --beacon HZ      CW beacon frequency (default 10489500000)\n"
                 "  --search HZ      beacon search half width (default 25000)\n"
                 "  --output FILE    48 kHz mono s16le audio, '-' for stdout\n"
                 "  --wisdom DIR     cache FFTW plans in DIR (recommended on the board)\n"
                 "  --quiet          no status lines on stderr\n");
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "nbd: %s needs a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--input") o.input = next("--input");
        else if (a == "--output") o.output = next("--output");
        else if (a == "--format") o.format = next("--format");
        else if (a == "--rate") o.rate = std::atof(next("--rate"));
        else if (a == "--center") o.center_hz = std::atof(next("--center"));
        else if (a == "--freq") o.freq_hz = std::atof(next("--freq"));
        else if (a == "--beacon") o.beacon_hz = std::atof(next("--beacon"));
        else if (a == "--search") o.search_hz = std::atof(next("--search"));
        else if (a == "--wisdom") o.wisdom_dir = next("--wisdom");
        else if (a == "--quiet") o.quiet = true;
        else if (a == "--source") o.source = next("--source");
        else if (a == "--ws") o.ws_port = std::atoi(next("--ws"));
        else if (a == "--ws-bind") o.ws_bind = next("--ws-bind");
        else if (a == "--bitrate") o.bitrate = std::atoi(next("--bitrate"));
        else if (a == "--opus-complexity") o.opus_complexity = std::atoi(next("--opus-complexity"));
        else if (a == "--realtime") o.realtime = true;
        else if (a == "--loop") o.loop = true;
        else if (a == "--spectrum") o.spectrum = next("--spectrum");
        else if (a == "--dsp-size") o.dsp_size = std::atoi(next("--dsp-size"));
        else if (a == "--tx") o.tx = true;
        else if (a == "--tx-lo") o.tx_lo_hz = std::atof(next("--tx-lo"));
        else if (a == "--tx-file") { o.tx_file = next("--tx-file"); o.tx = true; }
        else if (a == "--tx-max") o.tx_max_s = std::atof(next("--tx-max"));
        else if (a == "--mqtt") o.mqtt_host = next("--mqtt");
        else if (a == "--iiod-port") o.pluto.iiod_port = std::atoi(next("--iiod-port"));
        else if (a == "--http-port") o.pluto.http_port = std::atoi(next("--http-port"));
        else if (a == "--lnb-lo") o.pluto.lnb_lo_hz = std::atof(next("--lnb-lo"));
        else if (a == "--lo-offset") o.pluto.lo_offset_hz = std::atof(next("--lo-offset"));
        else if (a == "--duration") o.duration_s = std::atof(next("--duration"));
        else if (a == "--gain") {
            const std::string g = next("--gain");
            char* end = nullptr;
            const double db = std::strtod(g.c_str(), &end);
            if (end != g.c_str() && *end == '\0') {
                o.pluto.gain_mode = "manual";
                o.pluto.gain_db = db;
            } else {
                o.pluto.gain_mode = g;
            }
        }
        else if (a == "--help" || a == "-h") { usage(); std::exit(0); }
        else {
            std::fprintf(stderr, "nbd: unknown option %s\n", a.c_str());
            usage();
            return false;
        }
    }
    if (!o.source.empty()) {
        if (o.source.compare(0, 5, "pluto") != 0 || (o.source.size() > 5 && o.source[5] != ':')) {
            std::fprintf(stderr, "nbd: --source must be pluto or pluto:HOST\n");
            return false;
        }
        if (o.source.size() > 6) o.pluto.host = o.source.substr(6);
        o.pluto.center_hz = o.center_hz;
        o.rate = static_cast<double>(o.pluto.input_rate) / o.pluto.decimation;
    }
    if (o.spectrum != "auto" && o.spectrum != "maia" && o.spectrum != "local" && o.spectrum != "off") {
        std::fprintf(stderr, "nbd: --spectrum must be auto, maia, local or off\n");
        return false;
    }
    if (o.spectrum == "maia" && o.source.empty()) {
        std::fprintf(stderr, "nbd: --spectrum maia needs --source pluto\n");
        return false;
    }
    if (o.tx && o.ws_port <= 0) {
        std::fprintf(stderr, "nbd: --tx needs --ws (the microphone comes from the app)\n");
        return false;
    }
    if (o.tx && o.tx_file.empty() && o.source.empty()) {
        std::fprintf(stderr, "nbd: --tx needs --source pluto, or --tx-file\n");
        return false;
    }
    if (o.tx && o.mqtt_host.empty() && !o.source.empty()) o.mqtt_host = o.pluto.host;
    if (o.format != "cs16" && o.format != "cf32") {
        std::fprintf(stderr, "nbd: --format must be cs16 or cf32\n");
        return false;
    }
    if (o.rate <= 0 || std::fmod(o.rate, 375.0) != 0.0) {
        // 4096-sample blocks must map to a whole number of 48 kHz samples.
        std::fprintf(stderr, "nbd: --rate must be a multiple of 375 Hz (768000 recommended)\n");
        return false;
    }
    return true;
}

// Reads exactly n complex samples; returns false at end of input.
bool read_file_block(FILE* f, const std::string& format, std::vector<nb::cf>& out, size_t n) {
    out.resize(n);
    if (format == "cf32") return std::fread(out.data(), sizeof(nb::cf), n, f) == n;
    static std::vector<int16_t> raw;
    raw.resize(2 * n);
    if (std::fread(raw.data(), sizeof(int16_t), 2 * n, f) != 2 * n) return false;
    constexpr float k = 1.0f / 2048.0f;
    for (size_t i = 0; i < n; ++i) out[i] = nb::cf(raw[2 * i] * k, raw[2 * i + 1] * k);
    return true;
}

// Arduino PTT off through the board's mosquitto_pub, fire and forget (the
// RF is already muted by then; this releases the PA relay too).
void arduino_ptt_off(const std::string& host) {
    if (host.empty()) return;
    std::string h = host;
    const char* argv[] = {"mosquitto_pub", "-h", h.c_str(), "-t", "arduino/ptt", "-m", "off", nullptr};
    pid_t pid = 0;
    if (posix_spawnp(&pid, "mosquitto_pub", nullptr, nullptr, const_cast<char* const*>(argv), environ) == 0) {
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

// Cuts the board's DMA buffers (16384 samples) into WDSP blocks.
class LiveReader {
public:
    explicit LiveReader(nb::PlutoSource& src) : src_(src) {}
    bool read(std::vector<nb::cf>& out, size_t n) {
        while (pending_.size() - pos_ < n) {
            if (g_stop) return false;
            pending_.erase(pending_.begin(), pending_.begin() + static_cast<long>(pos_));
            pos_ = 0;
            src_.read(chunk_);
            pending_.insert(pending_.end(), chunk_.begin(), chunk_.end());
        }
        out.assign(pending_.begin() + static_cast<long>(pos_), pending_.begin() + static_cast<long>(pos_ + n));
        pos_ += n;
        return true;
    }

private:
    nb::PlutoSource& src_;
    std::vector<nb::cf> chunk_, pending_;
    size_t pos_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, o)) return 2;

    const bool live = !o.source.empty();
    std::unique_ptr<nb::PlutoSource> pluto;
    std::unique_ptr<LiveReader> live_reader;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, on_signal);
    if (live) {
        try {
            pluto = std::make_unique<nb::PlutoSource>(o.pluto);
            pluto->start();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "nbd: %s\n", e.what());
            return 1;
        }
        if (std::fabs(pluto->output_rate() - o.rate) > 1.0) {
            std::fprintf(stderr, "nbd: DDC gives %.0f S/s, expected %.0f\n", pluto->output_rate(), o.rate);
            return 1;
        }
        if (!o.quiet) std::fprintf(stderr, "nbd: %s\n", pluto->summary().c_str());
        live_reader = std::make_unique<LiveReader>(*pluto);
    }
    FILE* in = live ? nullptr : o.input == "-" ? stdin : std::fopen(o.input.c_str(), "rb");
    if (!live && !in) {
        std::fprintf(stderr, "nbd: cannot open %s: %s\n", o.input.c_str(), std::strerror(errno));
        return 1;
    }
    FILE* out = nullptr;
    if (!o.output.empty()) {
        out = o.output == "-" ? stdout : std::fopen(o.output.c_str(), "wb");
        if (!out) {
            std::fprintf(stderr, "nbd: cannot open %s: %s\n", o.output.c_str(), std::strerror(errno));
            return 1;
        }
    }
    if (!o.wisdom_dir.empty()) nb::wdsp_wisdom(o.wisdom_dir.c_str());

    nb::BeaconLockConfig cfg;
    cfg.sample_rate = o.rate;
    cfg.beacon_offset_hz = o.beacon_hz - o.center_hz;
    cfg.search_half_width_hz = o.search_hz;
    if (std::abs(cfg.beacon_offset_hz) + cfg.search_half_width_hz > o.rate / 2) {
        std::fprintf(stderr, "nbd: the beacon search window falls outside the IQ bandwidth\n");
        return 2;
    }
    nb::BeaconLock lock(cfg);

    const int block = 4096;
    // Demodulator front end: when the IQ rate is a multiple of 48 kHz, shift
    // the station to 0 Hz and decimate to 48 kS/s before WDSP. WDSP then runs
    // without its resampler, which on the Cortex-A9 (no double-precision
    // SIMD) is the difference between keeping up and not.
    const unsigned fe_decim =
        (std::fmod(o.rate, 48000.0) == 0.0 && o.rate > 48000.0) ? static_cast<unsigned>(o.rate / 48000.0) : 1;
    const bool use_frontend = fe_decim > 1 && block % static_cast<int>(fe_decim) == 0;
    std::unique_ptr<nb::Downconverter> frontend;
    if (use_frontend) frontend = std::make_unique<nb::Downconverter>(o.rate, fe_decim, 6500.0);
    nb::WdspRx rx(0, use_frontend ? 48000 : static_cast<int>(o.rate),
                  use_frontend ? block / static_cast<int>(fe_decim) : block, o.dsp_size);
    std::vector<nb::cf> narrow;
    double dial_hz = o.freq_hz;  // USB dial; the uplink follows it
    double rit_hz = 0;           // receive offset from the dial
    double station_offset = o.freq_hz - o.center_hz;
    double filter_low = 200.0, filter_high = 2800.0;
    int agc = 3;
    int nr = 0;
    // NR in force: suspended while transmitting, the board has no CPU left
    // for it then (EMNR alone takes 85 % of a Cortex-A9 core).
    int nr_applied = 0;
    // Keep the station and its passband inside the IQ bandwidth.
    const double min_freq = o.center_hz - o.rate / 2 + 3000.0;
    const double max_freq = o.center_hz + o.rate / 2 - 6000.0;

    std::unique_ptr<nb::WsServer> ws;
    std::unique_ptr<nb::Transmitter> tx;
    nb::TxCalibrator calib(48000.0);
    double tune_hz = 1000.0;
    std::unique_ptr<nb::OpusAudio> opus;
    uint16_t audio_seq = 0;
    bool opus_idle = true;

    // Waterfall: only for the app, and only computed while someone watches.
    std::string spectrum_source = o.ws_port > 0 ? o.spectrum : "off";
    if (spectrum_source == "auto") spectrum_source = live ? "maia" : "local";
    nb::SpectrumView view(o.center_hz, o.rate);
    std::unique_ptr<nb::SpectrumAnalyzer> analyzer;
    std::unique_ptr<nb::MaiaWaterfall> maia_wf;
    std::vector<float> spectrum_line;
    uint16_t spectrum_seq = 0;
    const bool maia_fallback = o.spectrum == "auto";
    auto use_local_spectrum = [&]() {
        maia_wf.reset();
        analyzer = std::make_unique<nb::SpectrumAnalyzer>(o.rate);
        analyzer->set_line_rate(view.view().rate);
        spectrum_source = "local";
    };
    if (spectrum_source == "maia") {
        maia_wf = std::make_unique<nb::MaiaWaterfall>(o.pluto.host, o.pluto.http_port);
        maia_wf->set_line_rate(view.view().rate);
        maia_wf->start();
    } else if (spectrum_source == "local") {
        use_local_spectrum();
    }
    if (o.ws_port > 0) {
        try {
            opus = std::make_unique<nb::OpusAudio>(o.bitrate, o.opus_complexity);
            ws = std::make_unique<nb::WsServer>();
            ws->set_hello(nb::hello_json(o.center_hz, o.rate, min_freq, max_freq, live ? o.source : "file",
                                         spectrum_source, o.tx));
            if (o.tx) {
                nb::TransmitterConfig tc;
                tc.max_tx_s = o.tx_max_s;
                nb::PlutoTxConfig pc;
                pc.host = o.pluto.host;
                pc.iiod_port = o.pluto.iiod_port;
                pc.lo_hz = o.tx_lo_hz;
                pc.expected_rate = static_cast<double>(o.pluto.input_rate);
                const std::string tx_file = o.tx_file;
                const double rate = static_cast<double>(o.pluto.input_rate);
                const double lo = o.tx_lo_hz;
                tx = std::make_unique<nb::Transmitter>(tc, [pc, tx_file, rate, lo]() -> std::unique_ptr<nb::TxSink> {
                    if (!tx_file.empty()) return std::make_unique<nb::FileTxSink>(tx_file, rate, lo, true);
                    return std::make_unique<nb::PlutoTx>(pc);
                });
                tx->set_dial(o.freq_hz);
                nb::WsServer* server = ws.get();
                const bool quiet = o.quiet;
                const std::string mqtt = o.mqtt_host;
                tx->on_status = [server, quiet](const nb::TxStatus& s) {
                    static std::string last;
                    server->broadcast_text(nb::tx_status_json(s));
                    if (!quiet && s.state != last) {
                        std::fprintf(stderr, "nbd: tx %s%s%s\n", s.state.c_str(), s.message.empty() ? "" : ": ",
                                     s.message.c_str());
                        last = s.state;
                    }
                };
                tx->on_emergency = [mqtt](const std::string&) { arduino_ptt_off(mqtt); };
                nb::Transmitter* t = tx.get();
                ws->on_binary = [t](int client, const std::string& m) {
                    uint16_t seq = 0;
                    std::string opus;
                    if (nb::parse_mic_packet(m, seq, opus)) t->mic_packet(client, seq, std::move(opus));
                };
                ws->on_close = [t](int client) { t->client_closed(client); };
            }
            ws->start(o.ws_bind, o.ws_port);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "nbd: %s\n", e.what());
            return 1;
        }
        if (!o.quiet) std::fprintf(stderr, "nbd: WebSocket on %s:%d\n", o.ws_bind.c_str(), ws->port());
    }
    auto apply_commands = [&]() {
        std::string text, error;
        int client = 0;
        while (ws && ws->next_command(text, client)) {
            error.clear();
            const nb::Command c = nb::parse_command(text, error);
            switch (c.type) {
            case nb::Command::Freq:
                if (c.a < min_freq || c.a > max_freq) error = "freq outside the received band";
                else {
                    dial_hz = c.a;
                    station_offset = std::clamp(dial_hz + rit_hz, min_freq, max_freq) - o.center_hz;
                }
                if (tx && error.empty()) tx->set_dial(c.a);
                calib.cancel();
                break;
            case nb::Command::Filter:
                filter_low = c.a;
                filter_high = c.b;
                rx.set_usb(filter_low, filter_high);
                break;
            case nb::Command::Rit:
                rit_hz = c.a;
                station_offset = std::clamp(dial_hz + rit_hz, min_freq, max_freq) - o.center_hz;
                calib.cancel();
                break;
            case nb::Command::TxEq:
                if (!tx) error = "this nbd has no transmitter (--tx)";
                else tx->set_eq(c.a, c.b);
                break;
            case nb::Command::Nr:
                nr = static_cast<int>(c.a);
                nr_applied = -1;  // applied below, unless transmitting
                break;
            case nb::Command::Agc:
                agc = static_cast<int>(c.a);
                rx.set_agc(agc);
                break;
            case nb::Command::Spectrum: {
                nb::SpectrumRequest r;
                r.center_hz = c.a;
                r.span_hz = c.b;
                r.bins = static_cast<int>(c.c);
                r.rate = c.d;
                const nb::SpectrumRequest& v = view.set(r);
                if (analyzer && v.rate > 0) analyzer->set_line_rate(v.rate);
                if (maia_wf && v.rate > 0) maia_wf->set_line_rate(v.rate);
                break;
            }
            case nb::Command::Bitrate:
                opus->set_bitrate(static_cast<int>(c.a));
                break;
            case nb::Command::Tx:
                if (!tx) error = "this nbd has no transmitter (--tx)";
                else error = tx->request(client, c.a != 0.0);
                break;
            case nb::Command::Tune:
                if (!tx) error = "this nbd has no transmitter (--tx)";
                else {
                    tx->set_tune(c.a != 0.0, c.b);
                    tune_hz = c.b;
                }
                break;
            case nb::Command::TxCalAuto: {
                if (!tx) {
                    error = "this nbd has no transmitter (--tx)";
                    break;
                }
                if (!frontend) {
                    error = "TX calibration needs the 48 kHz front end";
                    break;
                }
                const nb::TxStatus st = tx->status();
                if (st.state != "on" || !st.tune) error = "TX calibration: turn the tune carrier on and transmit first";
                else calib.start(tune_hz - rit_hz, st.cal_hz);  // with RIT the carrier sits lower
                break;
            }
            case nb::Command::TxCal:
                if (!tx) error = "this nbd has no transmitter (--tx)";
                else tx->set_cal(c.a);
                break;
            case nb::Command::MicGain:
                if (!tx) error = "this nbd has no transmitter (--tx)";
                else tx->set_mic_gain_db(c.a);
                break;
            case nb::Command::Comp:
                if (!tx) error = "this nbd has no transmitter (--tx)";
                else tx->set_compressor(c.a != 0.0, c.b);
                break;
            case nb::Command::None:
                break;
            }
            if (!error.empty()) {
                ws->broadcast_text("{\"type\":\"error\",\"message\":\"" + error + "\"}");
                if (!o.quiet) std::fprintf(stderr, "nbd: command refused: %s\n", error.c_str());
            }
        }
    };

    const auto wall_start = std::chrono::steady_clock::now();
    std::vector<nb::cf> iq;
    std::vector<float> audio;
    std::vector<int16_t> pcm;
    uint64_t total = 0;
    double next_report = 1.0;
    const uint64_t max_samples = o.duration_s > 0 ? static_cast<uint64_t>(o.duration_s * o.rate) : UINT64_MAX;
    int exit_code = 0;
    auto next_block = [&]() -> bool {
        if (total >= max_samples || g_stop) return false;
        if (!live) {
            if (read_file_block(in, o.format, iq, block)) return true;
            if (!o.loop || in == stdin || std::fseek(in, 0, SEEK_SET) != 0) return false;
            return read_file_block(in, o.format, iq, block);
        }
        try {
            return live_reader->read(iq, block);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "nbd: %s\n", e.what());
            exit_code = 1;
            return false;
        }
    };
    while (next_block()) {
        apply_commands();
        {
            const bool txing = tx && tx->status().state != "off" && tx->status().state != "error";
            const int want = txing ? 0 : nr;
            if (want != nr_applied) {
                rx.set_nr(want);
                nr_applied = want;
            }
        }
        lock.process(iq.data(), iq.size());
        // Before the first lock the correction is 0 (nominal LNB); after a
        // loss it keeps the last estimate.
        audio.clear();
        if (frontend) {
            frontend->set_frequency(station_offset + lock.correction_hz());
            narrow.clear();
            frontend->process(iq.data(), iq.size(), narrow);
            rx.process(narrow.data(), audio);
            if (calib.active() && tx) {
                const nb::TxStatus st = tx->status();
                const auto step = calib.feed(narrow.data(), narrow.size(), static_cast<double>(narrow.size()) / 48000.0,
                                             st.state == "on" && st.tune, lock.state() == nb::LockState::Locked);
                if (step == nb::TxCalibrator::Step::Done) {
                    tx->set_cal(calib.new_cal_hz());
                    char m[200];
                    std::snprintf(m, sizeof m, "{\"type\":\"txcal\",\"hz\":%.0f,\"measured\":%.1f,\"snr\":%.1f}",
                                  calib.new_cal_hz(), calib.measured_hz(), calib.snr_db());
                    if (ws) ws->broadcast_text(m);
                    if (!o.quiet) std::fprintf(stderr, "nbd: %s\n", calib.message().c_str());
                } else if (step == nb::TxCalibrator::Step::Failed) {
                    if (ws) ws->broadcast_text("{\"type\":\"error\",\"message\":\"" + calib.message() + "\"}");
                }
            }
        } else {
            rx.set_offset_hz(station_offset + lock.correction_hz());
            rx.process(iq.data(), audio);
        }
        if (out) {
            pcm.resize(audio.size());
            for (size_t i = 0; i < audio.size(); ++i) {
                const float s = std::fmax(-1.0f, std::fmin(1.0f, audio[i]));
                pcm[i] = static_cast<int16_t>(std::lrint(s * 32767.0f));
            }
            std::fwrite(pcm.data(), sizeof(int16_t), pcm.size(), out);
        }
        if (ws && ws->clients() > 0) {
            opus_idle = false;
            opus->push(audio.data(), audio.size(),
                       [&](const std::string& packet) { ws->broadcast_audio(nb::audio_packet(audio_seq++, packet)); });
        } else if (opus && !opus_idle) {
            opus->reset();
            opus_idle = true;
        }
        if (ws && ws->clients() > 0 && view.view().rate > 0) {
            bool got = false;
            if (analyzer) {
                analyzer->process(iq.data(), iq.size());
                got = analyzer->take_line(spectrum_line);
            } else if (maia_wf) {
                got = maia_wf->take_line(spectrum_line);
            }
            if (got) ws->broadcast_spectrum(view.pack(spectrum_line, lock.correction_hz(), spectrum_seq++));
        }
        total += iq.size();
        const double t = total / o.rate;
        if (maia_wf && maia_fallback && t > 5.0 && maia_wf->lines() == 0) {
            // Older Maia, or the spectrometer is busy elsewhere: do the FFT here.
            if (!o.quiet)
                std::fprintf(stderr, "nbd: no waterfall from maia (%s), computing it locally\n",
                             maia_wf->last_error().empty() ? "no line" : maia_wf->last_error().c_str());
            use_local_spectrum();
        }
        if (o.realtime && !live) {
            const auto due = wall_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                              std::chrono::duration<double>(t));
            std::this_thread::sleep_until(due);
        }
        if (t >= next_report) {
            next_report += 1.0;
            const auto st = lock.status();
            if (ws) {
                nb::StatusInfo si;
                si.freq_hz = o.center_hz + station_offset;
                si.beacon = nb::to_string(st.state);
                si.correction_hz = st.correction_hz;
                si.drift_hz_per_s = st.drift_hz_per_s;
                si.snr_db = st.acquisition_snr_db;
                si.signal_db = rx.signal_db();
                si.filter_low = filter_low;
                si.filter_high = filter_high;
                si.agc = agc;
                si.nr = nr;
                si.bitrate = opus->bitrate();
                si.clients = ws->clients();
                si.dropped = ws->dropped_audio_frames();
                si.uptime_s = t;
                ws->broadcast_text(nb::status_json(si));
                if (tx) ws->broadcast_text(nb::tx_status_json(tx->status()));
            }
            if (!o.quiet)
                std::fprintf(stderr,
                             "t=%7.1f s  beacon %-9s  correction %+10.1f Hz  drift %+6.2f Hz/s  snr %5.1f dB  S %6.1f dB"
                             "%s\n",
                             t, nb::to_string(st.state), st.correction_hz, st.drift_hz_per_s, st.acquisition_snr_db,
                             rx.signal_db(), ws ? ("  clients " + std::to_string(ws->clients())).c_str() : "");
        }
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
    if (!o.quiet && wall > 0.0) {
        // On a file this measures the CPU margin: above 1x, the board keeps up.
        std::fprintf(stderr, "nbd: %.1f s of IQ in %.1f s, %.2fx real time\n", total / o.rate, wall,
                     total / o.rate / wall);
    }
    if (ws) ws->stop();
    tx.reset();  // mutes the AD9363 and closes the TX buffer if it was on
    maia_wf.reset();
    if (pluto) {
        try {
            pluto->stop();  // gives the DMA back to the AD9363 for zynqrx / maia
        } catch (const std::exception& e) {
            std::fprintf(stderr, "nbd: stop: %s\n", e.what());
            exit_code = 1;
        }
    }
    if (out && out != stdout) std::fclose(out);
    if (in && in != stdin) std::fclose(in);
    return exit_code;
}
