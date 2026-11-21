// AX.25 frame decoder: any frame a TNC hands us.
#include <cstddef>
#include <cstdint>

#include "ogm/ax25.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    auto f = ogm::ax25::decode(ogm::ByteView(data, size));
    if (!f) return 0;
    (void)f->tnc2();
    (void)f->fullyRepeated();
    // Calls decoded from the air may hold characters encode() refuses; when
    // encode() accepts the frame, decoding its output must give it back.
    auto bytes = ogm::ax25::encode(*f);
    if (!bytes) return 0;
    auto g = ogm::ax25::decode(*bytes);
    if (!g || g->control != f->control || g->info != f->info || g->cr != f->cr || g->path.size() != f->path.size() ||
        !g->src.sameStation(f->src) || !g->dest.sameStation(f->dest))
        __builtin_trap();
    return 0;
}
