// The audio filter chosen by the client ({"cmd":"filter"}) must shape what
// is heard with NR off as well as on: a tone inside the passband passes, one
// outside it (or in LSB) is cut.
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

#include "wdsp_rx.h"

namespace {
constexpr double kPi = 3.14159265358979323846;

// Audio level (dB) of a complex tone at `hz` from the station, AGC off.
double level_db(double hz, double low, double high, int nr) {
    nb::WdspRx rx(0, 48000, 4096);
    rx.set_agc(0);
    rx.set_nr(nr);
    rx.set_usb(low, high);
    std::vector<nb::cf> in(4096);
    std::vector<float> audio;
    double ph = 0.0;
    for (int b = 0; b < 30; ++b) {
        for (auto& s : in) {
            s = nb::cf(static_cast<float>(0.01 * std::cos(ph)), static_cast<float>(0.01 * std::sin(ph)));
            ph = std::fmod(ph + 2.0 * kPi * hz / 48000.0, 2.0 * kPi);
        }
        rx.process(in.data(), audio);
    }
    double e = 0.0;
    for (size_t i = audio.size() / 2; i < audio.size(); ++i) e += audio[i] * audio[i];
    return 10.0 * std::log10(e / (audio.size() - audio.size() / 2) + 1e-30);
}
}  // namespace

int main() {
    struct Case { double hz, low, high; bool pass; };
    const Case cases[] = {
        {1000, 400, 2200, true},  {3000, 400, 2200, false}, {250, 400, 2200, false},
        {3000, 100, 3300, true},  {3800, 100, 3300, false}, {-1000, 100, 3300, false},
    };
    int failed = 0;
    for (int nr : {0, 1}) {
        const double ref = level_db(1000, 200, 2800, nr);
        for (const Case& c : cases) {
            const double rel = level_db(c.hz, c.low, c.high, nr) - ref;
            const bool ok = c.pass ? rel > -3.0 : rel < -40.0;
            std::printf("NR%d filter %4.0f-%4.0f, tone %+5.0f Hz: %7.1f dB %s\n", nr, c.low, c.high, c.hz, rel,
                        ok ? "ok" : "FAIL");
            if (!ok) ++failed;
        }
    }
    if (failed) {
        std::printf("FAIL: %d case(s)\n", failed);
        return 1;
    }
    std::printf("filter test passed\n");
    return 0;
}
