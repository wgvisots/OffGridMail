// KISS decoder: arbitrary bytes from the radio's serial link, in arbitrary chunks.
#include <cstddef>
#include <cstdint>

#include "ogm/kiss.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;
    ogm::kiss::Decoder d;
    std::size_t chunk = (data[0] % 16) + 1, i = 1;
    while (i < size) {
        std::size_t n = chunk < size - i ? chunk : size - i;
        for (auto& item : d.feed(ogm::ByteView(data + i, n))) {
            if (item.kind == ogm::kiss::Decoder::Item::Kind::Frame) {
                // Whatever came out must survive a wrap/decode round trip.
                ogm::Bytes again = ogm::kiss::wrap(item.data, static_cast<std::uint8_t>(item.command >> 4));
                ogm::kiss::Decoder d2;
                auto items = d2.feed(again);
                if (items.size() != 1 || items[0].data != item.data) __builtin_trap();
            }
        }
        i += n;
    }
    return 0;
}
