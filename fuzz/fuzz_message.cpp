// Winlink message parser and the header/charset helpers it uses.
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ogm/message.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::string_view text(reinterpret_cast<const char*>(data), size);
    (void)ogm::decodeHeaderWords(text);
    (void)ogm::utf8ToLatin1(text);
    (void)ogm::isValidUtf8(text);
    (void)ogm::parseDate(text);
    (void)ogm::normalizeAddress(text);

    auto m = ogm::Message::parse(ogm::ByteView(data, size));
    if (!m) return 0;
    (void)m->subject();
    (void)m->bodyText();
    (void)m->validate();
    (void)ogm::accountActivationPassword(*m);
    // Serializing a parsed message and parsing it again keeps its content.
    auto again = ogm::Message::parse(m->serialize());
    if (!again || again->mid() != m->mid() || again->rawBody() != m->rawBody() ||
        again->files().size() != m->files().size())
        __builtin_trap();
    return 0;
}
