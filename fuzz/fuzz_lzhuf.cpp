// LZHUF: decompress untrusted data (a gateway's message blocks), and check
// that anything we compress comes back unchanged.
#include <cstddef>
#include <algorithm>
#include <cstdint>

#include "ogm/lzhuf.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;
    bool b2 = (data[0] & 1) != 0;
    ogm::ByteView in(data + 1, size - 1);
    (void)ogm::lzhuf::decompress(in, b2);
    // Without the CRC every input reaches the decoder proper.
    (void)ogm::lzhuf::decompress(in, false);

    ogm::Bytes packed = ogm::lzhuf::compress(in, b2);
    auto back = ogm::lzhuf::decompress(packed, b2);
    if (!back || back->size() != in.size() || !std::equal(back->begin(), back->end(), in.begin())) __builtin_trap();
    return 0;
}
