#include "util.hpp"

#include <array>
#include <chrono>

namespace filexfer {

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint32_t crc32(const void* data, size_t n, uint32_t crc) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t c = ~crc;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

uint64_t fnv1a64(const void* data, size_t n, uint64_t h) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}

std::string printable(const std::string& s) {
    std::string r = s;
    for (char& ch : r)
        if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F) ch = '?';
    return r;
}

}  // namespace filexfer
