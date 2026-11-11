#include "pluto_source.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

#include "http_client.h"

namespace nb {

namespace {
constexpr uint32_t kRouteRegister = 0x800000BCu;  // bit 31: AXI core register, not AD9361 SPI
constexpr uint32_t kRouteEnable = 1u;

const IioDeviceInfo& find_device(const std::vector<IioDeviceInfo>& devs, const char* name) {
    for (const auto& d : devs)
        if (d.name == name) return d;
    throw IiodError(std::string("IIO device ") + name + " not found");
}

const IioChannelInfo& find_channel(const IioDeviceInfo& dev, const char* id, bool output) {
    const IioChannelInfo* c = dev.channel(id, output);
    if (!c) throw IiodError(std::string("channel ") + id + " not found on " + dev.name);
    return *c;
}
}  // namespace

PlutoPlan make_plan(const PlutoConfig& cfg) {
    PlutoPlan p;
    p.rx_lo_hz = static_cast<int64_t>(std::llround(cfg.center_hz - cfg.lnb_lo_hz + cfg.lo_offset_hz));
    // Maia: the DDC frequency is the one shifted to baseband, relative to the LO.
    p.ddc_frequency_hz = -cfg.lo_offset_hz;
    p.output_rate = static_cast<double>(cfg.input_rate) / cfg.decimation;
    return p;
}

std::string ddc_design_json(const PlutoConfig& cfg) {
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "{\"frequency\":%.1f,\"decimation\":%d,\"transition_bandwidth\":0.1,"
                  "\"passband_ripple\":0.01,\"stopband_attenuation_db\":60.0,\"stopband_one_over_f\":true}",
                  make_plan(cfg).ddc_frequency_hz, cfg.decimation);
    return buf;
}

PlutoSource::PlutoSource(PlutoConfig cfg) : cfg_(std::move(cfg)) {
    if (cfg_.buffer_samples == 0 || cfg_.buffer_samples % 4096 != 0)
        throw std::invalid_argument("buffer_samples must be a multiple of 4096");
    if (cfg_.decimation < 2) throw std::invalid_argument("DDC decimation must be at least 2");
}

PlutoSource::~PlutoSource() {
    try {
        stop();
    } catch (...) {
    }
}

void PlutoSource::start() {
    const PlutoPlan plan = make_plan(cfg_);
    iiod_.connect(cfg_.host, cfg_.iiod_port);
    const std::string ver = iiod_.version();
    const auto devs = iiod_.context();
    phy_ = find_device(devs, "ad9361-phy");
    adc_ = find_device(devs, "cf-ad9361-lpc");
    const auto& rx = find_channel(phy_, "voltage0", false);
    const auto& lo = find_channel(phy_, "altvoltage0", true);

    iiod_.write_channel_attr(phy_, rx, "rf_bandwidth", std::to_string(cfg_.rf_bandwidth));
    iiod_.write_channel_attr(phy_, rx, "sampling_frequency", std::to_string(cfg_.input_rate));
    const long actual_rate = std::atol(iiod_.read_channel_attr(phy_, rx, "sampling_frequency").c_str());
    if (actual_rate <= 0 || std::labs(actual_rate - cfg_.input_rate) > cfg_.input_rate / 1000)
        throw IiodError("AD9363 refused " + std::to_string(cfg_.input_rate) + " S/s (got " +
                        std::to_string(actual_rate) + ")");
    iiod_.write_channel_attr(phy_, lo, "frequency", std::to_string(plan.rx_lo_hz));
    iiod_.write_channel_attr(phy_, rx, "gain_control_mode", cfg_.gain_mode);
    if (cfg_.gain_mode == "manual") {
        char g[32];
        std::snprintf(g, sizeof g, "%.1f", cfg_.gain_db);
        iiod_.write_channel_attr(phy_, rx, "hardwaregain", g);
    }

    // DDC design (maia-httpd computes the FIR, can take a couple of seconds).
    const HttpResponse put = http_request(cfg_.host, cfg_.http_port, "PUT", "/api/ddc/design", ddc_design_json(cfg_));
    if (put.status < 200 || put.status > 299)
        throw std::runtime_error("maia DDC design refused: HTTP " + std::to_string(put.status) + " " +
                                 put.body.substr(0, 200));
    const HttpResponse get = http_request(cfg_.host, cfg_.http_port, "GET", "/api/ddc/config");
    double dec = 0.0, out_rate = 0.0;
    if (get.status != 200 || !json_number(get.body, "decimation", dec) || static_cast<int>(dec) != cfg_.decimation)
        throw std::runtime_error("maia DDC did not take decimation " + std::to_string(cfg_.decimation));
    output_rate_ = json_number(get.body, "output_sampling_frequency", out_rate) && out_rate > 0 ? out_rate
                                                                                                : plan.output_rate;
    if (cfg_.spectrometer_on_ddc) {
        try {
            http_request(cfg_.host, cfg_.http_port, "PATCH", "/api/spectrometer", "{\"input\":\"DDC\"}");
        } catch (const std::exception&) {
            // The waterfall is a convenience; IQ streaming does not need it.
        }
    }

    set_route(true);
    iiod_.set_kernel_buffers(adc_, 8);
    const auto& i_ch = find_channel(adc_, "voltage0", false);
    const auto& q_ch = find_channel(adc_, "voltage1", false);
    mask_ = iio_channel_mask(adc_.channels.size(), {i_ch.index_in_device, q_ch.index_in_device});
    iiod_.open_buffer(adc_, cfg_.buffer_samples, mask_);
    open_ = true;
    raw_.assign(cfg_.buffer_samples * 4, 0);

    char s[256];
    std::snprintf(s, sizeof s, "iiod %s, AD9363 LO %.6f MHz at %.3f MS/s, DDC %+.0f Hz / %d -> %.0f S/s",
                  ver.c_str(), plan.rx_lo_hz / 1e6, actual_rate / 1e6, plan.ddc_frequency_hz, cfg_.decimation,
                  output_rate_);
    summary_ = s;
}

void PlutoSource::set_route(bool enabled) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint32_t cur = iiod_.read_register(adc_, kRouteRegister);
        const uint32_t want = enabled ? (cur | kRouteEnable) : (cur & ~kRouteEnable);
        if (want != cur) iiod_.write_register(adc_, kRouteRegister, want);
        if (((iiod_.read_register(adc_, kRouteRegister) & kRouteEnable) != 0) == enabled) {
            routed_ = enabled;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw IiodError(enabled ? "could not route the DDC to the DMA" : "could not release the DDC route");
}

void PlutoSource::read(std::vector<cf>& out) {
    if (!open_) throw IiodError("RX buffer not open");
    size_t got = 0;
    while (got < raw_.size()) {
        std::vector<uint8_t> chunk(raw_.size() - got);
        const size_t n = iiod_.read_buffer(adc_, chunk, mask_.size() / 8);
        if (n == 0) throw IiodError("iiod returned no samples");
        std::memcpy(raw_.data() + got, chunk.data(), n);
        got += n;
    }
    const size_t samples = raw_.size() / 4;
    out.resize(samples);
    constexpr float k = 1.0f / 2048.0f;  // 12-bit samples in 16-bit words
    for (size_t i = 0; i < samples; ++i) {
        int16_t iv, qv;
        std::memcpy(&iv, &raw_[4 * i], 2);
        std::memcpy(&qv, &raw_[4 * i + 2], 2);
        out[i] = cf(iv * k, qv * k);
    }
}

void PlutoSource::stop() {
    if (open_) {
        open_ = false;
        try {
            iiod_.close_buffer(adc_);
        } catch (const std::exception&) {
        }
    }
    if (routed_) set_route(false);
    iiod_.close();
}

}  // namespace nb
