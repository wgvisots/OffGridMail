// AX.25 connected mode: a stream of frames from anyone on the channel,
// interleaved with time passing and with data to send.
//
// Input: records of [kind][len][bytes...].  kind % 4: 0 = a frame to decode
// and feed, 1 = time passes (len * 100 ms), 2 = write len bytes, 3 = control.
#include <cstddef>
#include <cstdint>

#include "ogm/ax25_link.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;
    ogm::ax25::LinkConfig cfg;
    cfg.mycall = *ogm::ax25::Address::parse("N0CALL");
    cfg.acceptIncoming = (data[0] & 1) != 0;
    cfg.maxframe = (data[0] >> 1) % 8;
    cfg.paclen = 16 + data[0];
    ogm::ax25::Link link(cfg);
    ogm::TimeMs now = 0;
    if (data[0] & 0x80) link.connect(*ogm::ax25::Address::parse("W1AW-10"), {}, now);

    std::size_t i = 1;
    while (i + 2 <= size) {
        std::uint8_t kind = data[i] % 4, len = data[i + 1];
        i += 2;
        std::size_t n = len < size - i ? len : size - i;
        switch (kind) {
            case 0:
                if (auto f = ogm::ax25::decode(ogm::ByteView(data + i, n))) link.onFrame(*f, now);
                break;
            case 1: now += static_cast<ogm::TimeMs>(len) * 100; break;
            case 2: link.write(ogm::ByteView(data + i, n), now); break;
            case 3:
                if (len % 3 == 0) link.disconnect(now);
                else if (len % 3 == 1) link.abort(now);
                else link.connect(*ogm::ax25::Address::parse("W1AW-10"), {}, now);
                break;
        }
        if (kind == 0 || kind == 2) i += n;
        link.tick(now);
        for (const auto& f : link.takeOutgoing())
            if (!ogm::ax25::encode(f)) __builtin_trap();  // everything we send must encode
        (void)link.takeEvents();
        (void)link.queuedBytes();
    }
    return 0;
}
