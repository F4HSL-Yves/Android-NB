// Waterfall: FFT, analyzer levels and the packed line sent to the app.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "spectrum.h"

namespace {

int failures = 0;
#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                          \
            std::printf("\n");                                 \
            ++failures;                                        \
        }                                                      \
    } while (0)

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 768000.0;
constexpr double kCenter = 10489750000.0;

struct Line {
    int seq = 0;
    double center_off = 0, span = 0, ref_db = 0, step_db = 0;
    std::vector<int> v;
};

Line unpack(const std::string& p) {
    auto u = [&](size_t i) { return static_cast<unsigned>(static_cast<uint8_t>(p[i])); };
    Line l;
    if (p.size() < 14 || u(0) != 0x02) return l;
    l.seq = static_cast<int>(u(1) << 8 | u(2));
    l.center_off = static_cast<int32_t>(u(3) << 24 | u(4) << 16 | u(5) << 8 | u(6));
    l.span = static_cast<double>(u(7) << 24 | u(8) << 16 | u(9) << 8 | u(10));
    l.ref_db = static_cast<int16_t>(u(11) << 8 | u(12)) / 10.0;
    l.step_db = u(13) / 10.0;
    for (size_t i = 14; i < p.size(); ++i) l.v.push_back(static_cast<int>(u(i)));
    return l;
}

int argmax(const std::vector<int>& v) {
    int best = 0;
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i] > v[static_cast<size_t>(best)]) best = static_cast<int>(i);
    return best;
}

std::vector<nb::cf> tone(double hz, double amp, size_t n, double noise = 1e-4) {
    std::vector<nb::cf> x(n);
    unsigned seed = 1;
    auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return ((seed >> 8) & 0xFFFF) / 65536.0 - 0.5; };
    for (size_t i = 0; i < n; ++i) {
        const double ph = 2 * kPi * hz * static_cast<double>(i) / kRate;
        x[i] = nb::cf(static_cast<float>(amp * std::cos(ph) + noise * rnd()), static_cast<float>(amp * std::sin(ph) + noise * rnd()));
    }
    return x;
}

void test_fft() {
    const size_t n = 1024;
    std::vector<nb::cf> a(n);
    std::vector<std::complex<double>> ref(n);
    for (size_t i = 0; i < n; ++i) {
        a[i] = nb::cf(static_cast<float>(std::sin(0.37 * i)), static_cast<float>(std::cos(1.3 * i * i * 1e-3)));
        ref[i] = std::complex<double>(a[i].real(), a[i].imag());
    }
    nb::Fft(n).forward(a.data());
    nb::fft_inplace(ref);
    double err = 0;
    for (size_t i = 0; i < n; ++i) err = std::fmax(err, std::abs(std::complex<double>(a[i].real(), a[i].imag()) - ref[i]));
    CHECK(err < 1e-3, "float fft differs from reference by %g", err);
}

void test_analyzer_level_and_position() {
    nb::SpectrumAnalyzer an(kRate);
    an.set_line_rate(8);
    const auto x = tone(100000.0, 0.5, static_cast<size_t>(kRate));  // 1 s
    std::vector<float> line;
    int lines = 0;
    for (size_t i = 0; i + 4096 <= x.size(); i += 4096) {
        an.process(x.data() + i, 4096);
        if (an.take_line(line)) ++lines;
    }
    CHECK(lines >= 7 && lines <= 9, "%d lines in 1 s at 8 lines/s", lines);
    CHECK(line.size() == 4096, "line has %zu bins", line.size());
    size_t peak = 0;
    for (size_t i = 1; i < line.size(); ++i)
        if (line[i] > line[peak]) peak = i;
    const double peak_hz = (static_cast<double>(peak) - 2048.0) * kRate / 4096.0;
    CHECK(std::fabs(peak_hz - 100000.0) < kRate / 4096.0, "tone at %.0f Hz, expected 100000", peak_hz);
    const double db = 10 * std::log10(line[peak]);
    CHECK(std::fabs(db - (-6.02)) < 1.5, "tone level %.2f dBFS, expected -6", db);
}

void test_view_full_band_and_correction() {
    nb::SpectrumView view(kCenter, kRate);
    // A line with one hot bin at raw +120 kHz.
    std::vector<float> line(4096, 1e-10f);
    const size_t hot = 2048 + static_cast<size_t>(120000.0 / (kRate / 4096));
    line[hot] = 1e-2f;
    const Line l = unpack(view.pack(line, 0.0, 7));
    CHECK(l.seq == 7, "seq %d", l.seq);
    CHECK(l.center_off == 0 && l.span == kRate, "view %+.0f / %.0f", l.center_off, l.span);
    CHECK(l.v.size() == 768, "%zu bins", l.v.size());
    CHECK(std::fabs(l.step_db - 0.5) < 1e-9, "step %.2f", l.step_db);
    const double f = -kRate / 2 + (argmax(l.v) + 0.5) * kRate / 768;
    CHECK(std::fabs(f - 120000.0) < kRate / 768, "peak at %.0f Hz", f);
    const double peak_db = l.ref_db + l.v[static_cast<size_t>(argmax(l.v))] * l.step_db;
    CHECK(std::fabs(peak_db - (-20.0)) < 0.6, "peak %.1f dB, expected -20", peak_db);
    CHECK(std::fabs(l.ref_db - (-100.0)) < 0.2, "floor %.1f dB, expected -100", l.ref_db);
    // LNB 20 kHz high: the same raw bin is the station at +100 kHz.
    const Line c = unpack(view.pack(line, 20000.0, 8));
    const double fc = -kRate / 2 + (argmax(c.v) + 0.5) * kRate / 768;
    CHECK(std::fabs(fc - 100000.0) < kRate / 768, "corrected peak at %.0f Hz, expected 100000", fc);
}

void test_view_zoom_and_clamp() {
    nb::SpectrumView view(kCenter, kRate);
    std::vector<float> line(4096, 1e-10f);
    line[2048 + 640] = 1e-3f;  // +120 kHz
    nb::SpectrumRequest r;
    r.center_hz = kCenter + 120000.0;
    r.span_hz = 24000.0;
    r.bins = 256;
    r.rate = 4;
    const nb::SpectrumRequest& v = view.set(r);
    CHECK(v.span_hz == 24000.0 && v.bins == 256 && v.rate == 4, "zoom refused");
    const Line l = unpack(view.pack(line, 0.0, 0));
    CHECK(l.center_off == 120000.0 && l.span == 24000.0, "view %+.0f / %.0f", l.center_off, l.span);
    const int m = argmax(l.v);
    // One 187.5 Hz input bin covers 2 output bins of 93.75 Hz around the middle.
    CHECK(m >= 126 && m <= 129, "zoomed peak at bin %d of 256", m);
    // Out of range requests are pulled back inside the band.
    r.center_hz = kCenter + 1e6;
    r.span_hz = 1e9;
    r.bins = 100000;
    r.rate = 100;
    const nb::SpectrumRequest& w = view.set(r);
    CHECK(w.span_hz == kRate && w.center_hz == kCenter && w.bins == 2048 && w.rate == 25, "clamp: %.0f %.0f %d %.0f",
          w.span_hz, w.center_hz, w.bins, w.rate);
}

}  // namespace

int main() {
    test_fft();
    test_analyzer_level_and_position();
    test_view_full_band_and_correction();
    test_view_zoom_and_clamp();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("spectrum: all tests passed\n");
    return 0;
}
