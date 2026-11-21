// B2F session: whatever a gateway (or a station calling us) might send.
// The first byte picks the role and the chunk size; the rest is the stream.
#include <cstddef>
#include <cstdint>

#include "ogm/b2f.hpp"
#include "ogm/message.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;
    ogm::b2f::Config cfg;
    cfg.mycall = "N0CALL";
    cfg.targetcall = "W1AW-10";
    cfg.password = (data[0] & 0x40) ? "secret" : "";
    cfg.master = (data[0] & 0x80) != 0;
    if (cfg.master) cfg.challenge = "12345678";

    std::vector<ogm::b2f::Outgoing> out;
    if (data[0] & 0x20) {
        ogm::Message::Draft d;
        d.from = "N0CALL";
        d.to = {"W1AW"};
        d.subject = "fuzz";
        d.body = "body\n";
        auto m = ogm::Message::compose(d, 0, 1);
        out.push_back({m.mid(), m.header("Subject"), m.serialize()});
    }
    ogm::b2f::Session s(cfg, out, [](const std::string& mid) { return mid == "HAVEIT"; });
    s.setStore([&](const std::string&, const ogm::Bytes&, std::string&) { return (data[0] & 0x10) == 0; });
    s.start();
    std::size_t chunk = (data[0] & 0x0F) + 1, i = 1;
    while (i < size && !s.finished()) {
        std::size_t n = chunk < size - i ? chunk : size - i;
        s.onData(ogm::ByteView(data + i, n));
        (void)s.takeOut();
        (void)s.takeEvents();
        i += n;
    }
    return 0;
}
