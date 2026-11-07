// Tiny blocking HTTP/1.1 client, enough for maia-httpd's JSON API on the
// board (PUT /api/ddc/design, GET /api/ddc/config, PATCH /api/spectrometer).
#pragma once

#include <string>

namespace nb {

struct HttpResponse {
    int status = 0;
    std::string body;
};

HttpResponse http_request(const std::string& host, int port, const std::string& method, const std::string& path,
                          const std::string& json_body = std::string(), int timeout_ms = 15000);

// Value of a numeric field in a flat JSON object, e.g. "decimation": 4.
// Returns false when absent.
bool json_number(const std::string& json, const std::string& key, double& value);

}  // namespace nb
