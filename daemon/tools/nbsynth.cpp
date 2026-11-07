// nbsynth: writes a synthetic narrowband transponder IQ stream (cs16) for
// testing nbd without an antenna, and for measuring CPU load on the board:
//   nbsynth --seconds 30 > /tmp/test.cs16 && nbd --input /tmp/test.cs16 --output /dev/null
// Content: the CW beacon with a given LNB error and drift, plus a USB
// station sending a 1000 Hz tone at the --freq dial, in white noise.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    double fs = 768000.0, seconds = 30.0, center = 10489750000.0, beacon = 10489500000.0,
           dial = 10489900000.0, err0 = 2500.0, drift = 1.0;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        const double v = std::atof(argv[i + 1]);
        if (a == "--rate") fs = v;
        else if (a == "--seconds") seconds = v;
        else if (a == "--center") center = v;
        else if (a == "--freq") dial = v;
        else if (a == "--lnb-error") err0 = v;
        else if (a == "--drift") drift = v;
        else {
            std::fprintf(stderr, "usage: nbsynth [--seconds S] [--rate HZ] [--center HZ] [--freq HZ] "
                                 "[--lnb-error HZ] [--drift HZ_PER_S] > out.cs16\n");
            return 2;
        }
    }
    const double two_pi = 2.0 * 3.14159265358979323846;
    std::mt19937 rng(42);
    std::normal_distribution<double> gauss(0.0, 40.0);
    std::vector<int16_t> buf;
    double ph_b = 0.0, ph_s = 0.0;
    const size_t total = static_cast<size_t>(seconds * fs), block = 8192;
    for (size_t pos = 0; pos < total; pos += block) {
        buf.clear();
        for (size_t i = 0; i < block && pos + i < total; ++i) {
            const double t = (pos + i) / fs;
            const double e = err0 + drift * t;
            ph_b = std::fmod(ph_b + two_pi * (beacon - center + e) / fs, two_pi);
            ph_s = std::fmod(ph_s + two_pi * (dial + 1000.0 - center + e) / fs, two_pi);
            // 12-bit full scale is 2048: beacon at 400, station at 150.
            buf.push_back(static_cast<int16_t>(std::lrint(400 * std::cos(ph_b) + 150 * std::cos(ph_s) + gauss(rng))));
            buf.push_back(static_cast<int16_t>(std::lrint(400 * std::sin(ph_b) + 150 * std::sin(ph_s) + gauss(rng))));
        }
        std::fwrite(buf.data(), sizeof(int16_t), buf.size(), stdout);
    }
    return 0;
}
