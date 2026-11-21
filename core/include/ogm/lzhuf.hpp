// LZHUF compression as used by the FBB B2 forwarding protocol (Winlink).
//
// LZSS with a 2 KB window and 60-byte lookahead, followed by adaptive
// Huffman coding (Okumura and Yoshizaki's LZHUF.C, 1988).  The B2 variant
// puts a CRC16 (XMODEM) of everything after it in front:
//
//     [crc16 LE, B2 only] [uncompressed length, 4 bytes LE] [coded bits...]
#pragma once

#include <optional>
#include <string>

#include "ogm/common.hpp"

namespace ogm::lzhuf {

Bytes compress(ByteView data, bool b2 = true);

// Returns nullopt on a bad checksum, truncated input, or nonsense lengths;
// error (if given) says which.
std::optional<Bytes> decompress(ByteView data, bool b2 = true, std::string* error = nullptr);

// CRC-16/XMODEM (poly 0x1021, init 0), the checksum the B2 header carries.
std::uint16_t crc16(ByteView data);

}  // namespace ogm::lzhuf
