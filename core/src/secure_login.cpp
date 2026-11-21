#include "ogm/secure_login.hpp"

#include <cstdint>
#include <cstdio>

#include "ogm/md5.hpp"

namespace ogm {
namespace {

// The fixed salt every Winlink client appends.  First published in
// paclink-unix; the same bytes appear in Pat's wl2k-go (MIT).
constexpr std::uint8_t kSalt[64] = {
    77,  197, 101, 206, 190, 249, 93,  200, 51,  243, 93,  237, 71,  94,  239, 138,
    68,  108, 70,  185, 225, 137, 217, 16,  51,  122, 193, 48,  194, 195, 198, 175,
    172, 169, 70,  84,  61,  62,  104, 186, 114, 52,  61,  168, 66,  129, 192, 208,
    187, 249, 232, 193, 41,  113, 41,  45,  240, 16,  29,  228, 208, 228, 61,  20};

}  // namespace

std::string secureLoginResponse(std::string_view challenge, std::string_view password) {
    Bytes payload;
    append(payload, challenge);
    append(payload, password);
    payload.insert(payload.end(), std::begin(kSalt), std::end(kSalt));
    auto sum = md5(payload);

    // 30-bit number from the first four digest bytes, little-endian.
    std::uint32_t pr = static_cast<std::uint32_t>(sum[3] & 0x3f) << 24 | static_cast<std::uint32_t>(sum[2]) << 16 |
                       static_cast<std::uint32_t>(sum[1]) << 8 | sum[0];
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08u", pr);
    std::string s(buf);
    return s.substr(s.size() - 8);
}

}  // namespace ogm
