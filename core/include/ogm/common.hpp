// Types shared by the whole core.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ogm {

using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;

// Milliseconds on whatever monotonic clock the shell uses.  The core never
// reads a clock itself; every call that cares about time is handed `now`.
using TimeMs = std::int64_t;

inline ByteView asBytes(std::string_view s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}
inline std::string_view asText(ByteView b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}
inline void append(Bytes& out, ByteView b) { out.insert(out.end(), b.begin(), b.end()); }
inline void append(Bytes& out, std::string_view s) { append(out, asBytes(s)); }

}  // namespace ogm
