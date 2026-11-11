// Spectrum lines from Maia's FPGA spectrometer (WebSocket /waterfall of
// maia-httpd), so the waterfall costs no FFT on the board's CPU.
//
// maia-httpd sends one binary message per line: 4096 float32 (little
// endian), linear power, index 0 = centre - fs/2, index 0 itself carrying a
// flag instead of power. nbd points the spectrometer at the DDC output, so
// the lines cover the same 768 kS/s band as the IQ stream.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nb {

class MaiaWaterfall {
public:
    MaiaWaterfall(std::string host, int http_port);
    ~MaiaWaterfall();
    MaiaWaterfall(const MaiaWaterfall&) = delete;
    MaiaWaterfall& operator=(const MaiaWaterfall&) = delete;

    void start();
    void stop();
    // Asks the spectrometer for this many lines per second (applied by the
    // background thread; it also changes the rate seen in Maia's web UI).
    void set_line_rate(double lines_per_s);
    // The newest line, if one arrived since the last call.
    bool take_line(std::vector<float>& power);
    // Lines received so far; 0 means Maia is not delivering (yet).
    uint64_t lines() const { return lines_.load(); }
    std::string last_error() const;

private:
    void run();
    void session();
    void apply_rate();

    std::string host_;
    int port_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> lines_{0};
    std::atomic<double> want_rate_{8.0};
    double applied_rate_ = 0.0;
    int fd_ = -1;
    mutable std::mutex mu_;
    std::vector<float> latest_;
    bool fresh_ = false;
    std::string error_;
};

}  // namespace nb
