// MD5 (RFC 1321).  Used only where the Winlink protocol requires it: the
// secure-login response and message IDs.  Not for anything security-related.
#pragma once

#include <array>
#include <cstdint>

#include "ogm/common.hpp"

namespace ogm {

std::array<std::uint8_t, 16> md5(ByteView data);

}  // namespace ogm
