#include <string>

#include "ogm/kiss.hpp"
#include "test.hpp"

using ogm::Bytes;
using namespace ogm::kiss;
using Kind = Decoder::Item::Kind;

namespace {
Bytes bytesOf(const std::string& s) { return Bytes(s.begin(), s.end()); }
}  // namespace

TEST(kiss_wrap_escapes_fend_and_fesc) {
    Bytes body{0x01, FEND, 0x02, FESC, 0x03};
    Bytes expect{FEND, 0x00, 0x01, FESC, TFEND, 0x02, FESC, TFESC, 0x03, FEND};
    CHECK_EQ(wrap(body), expect);
    CHECK_EQ(wrap(Bytes{0x41}, 1), (Bytes{FEND, 0x10, 0x41, FEND}));
}

TEST(kiss_exit_sequence) { CHECK_EQ(exitSequence(), (Bytes{0xC0, 0xFF, 0xC0})); }

TEST(kiss_roundtrip_one_byte_at_a_time) {
    Bytes body{0x10, FEND, FESC, 0x20, FESC, FESC, FEND};
    Bytes wire = wrap(body);
    Decoder d;
    std::vector<Decoder::Item> items;
    for (auto b : wire) {
        auto got = d.feed(std::span(&b, 1));
        items.insert(items.end(), got.begin(), got.end());
    }
    REQUIRE(items.size() == 1);
    CHECK(items[0].kind == Kind::Frame);
    CHECK_EQ(items[0].command, 0x00);
    CHECK_EQ(items[0].data, body);
}

TEST(kiss_shared_fend_between_frames) {
    Bytes a = wrap(Bytes{0x41});
    Bytes b = wrap(Bytes{0x42});
    a.pop_back();  // "C0 00 41 C0 00 42 C0"
    a.insert(a.end(), b.begin(), b.end());
    Decoder d;
    auto items = d.feed(a);
    REQUIRE(items.size() == 2);
    CHECK_EQ(items[0].data, Bytes{0x41});
    CHECK_EQ(items[1].data, Bytes{0x42});
}

TEST(kiss_text_outside_frames_is_reported) {
    Bytes wire = bytesOf("ID TH-D75\r");
    Bytes f = wrap(Bytes{0x41});
    wire.insert(wire.end(), f.begin(), f.end());
    Bytes tail = bytesOf("?\r");
    wire.insert(wire.end(), tail.begin(), tail.end());
    Decoder d;
    auto items = d.feed(wire);
    REQUIRE(items.size() == 3);
    CHECK(items[0].kind == Kind::Text);
    CHECK_EQ(items[0].data, bytesOf("ID TH-D75\r"));
    CHECK(items[1].kind == Kind::Frame);
    CHECK(items[2].kind == Kind::Text);
    CHECK_EQ(items[2].data, bytesOf("?\r"));
}

TEST(kiss_back_to_back_fends_are_ignored) {
    Decoder d;
    auto items = d.feed(Bytes{FEND, FEND, FEND, 0x00, 0x41, FEND, FEND});
    REQUIRE(items.size() == 1);
    CHECK_EQ(items[0].data, Bytes{0x41});
}

TEST(kiss_oversize_frame_is_dropped_and_decoder_recovers) {
    Bytes junk(Decoder::kMaxFrame + 50, 0x41);
    Bytes wire = wrap(junk);
    Bytes ok = wrap(Bytes{0x42});
    wire.insert(wire.end(), ok.begin(), ok.end());
    Decoder d;
    auto items = d.feed(wire);
    REQUIRE(items.size() == 1);
    CHECK_EQ(items[0].data, Bytes{0x42});
    CHECK_EQ(d.droppedFrames(), 1u);
}

TEST(kiss_command_frames_keep_their_command_byte) {
    Decoder d;
    auto items = d.feed(Bytes{FEND, 0x01, 0x32, FEND});  // TXDELAY 50
    REQUIRE(items.size() == 1);
    CHECK_EQ(items[0].command, 0x01);
    CHECK_EQ(items[0].data, Bytes{0x32});
}

TEST(kiss_cr_after_a_frame_is_text_not_a_frame) {
    // What the engine sends on connect: KISS exit, then "\r", then "ID\r".
    Bytes wire = exitSequence();
    for (char c : std::string("\rID\r")) wire.push_back(static_cast<std::uint8_t>(c));
    Decoder d;
    auto items = d.feed(wire);
    REQUIRE(items.size() == 3);
    CHECK(items[0].kind == Kind::Frame);
    CHECK_EQ(items[0].command, 0xFF);
    CHECK(items[1].kind == Kind::Text);
    CHECK_EQ(items[1].data, bytesOf("\r"));
    CHECK(items[2].kind == Kind::Text);
    CHECK_EQ(items[2].data, bytesOf("ID\r"));
}

TEST(kiss_parameter_command) {
    CHECK_EQ(parameter(kTxDelay, 30), (Bytes{FEND, 0x01, 30, FEND}));
    CHECK_EQ(parameter(kPersistence, 0xC0), (Bytes{FEND, 0x02, FESC, TFEND, FEND}));
}
