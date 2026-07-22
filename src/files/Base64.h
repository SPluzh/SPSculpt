#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace Base64 {

inline std::vector<uint8_t> decode(const std::string& in) {
    std::vector<uint8_t> out;
    std::vector<int> T(256, -1);
    const char* b64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; i++) {
        T[(uint8_t)b64_chars[i]] = i;
    }
    
    int val = 0;
    int valb = -8;
    for (uint8_t c : in) {
        if (T[c] == -1) continue;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    return out;
}

inline std::vector<uint8_t> decodeUri(const std::string& uri) {
    size_t comma = uri.find(',');
    std::string base64_data = (comma == std::string::npos) ? uri : uri.substr(comma + 1);
    return decode(base64_data);
}

} // namespace Base64
