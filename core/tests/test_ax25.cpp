#include <fstream>
#include <sstream>
#include <string>

#include "test.hpp"
#include "ogm/ax25.hpp"

using ogm::Bytes;
using namespace ogm::ax25;

namespace {

Bytes fromHex(const std::string& hex) {
    Bytes out;
    std::istringstream in(hex);
    std::string tok;
    while (in >> tok) out.push_back(static_cast<std::uint8_t>(std::stoul(tok, nullptr, 16)));
    return out;
}

Frame iFrame() {
    Frame f;
    f.dest = *Address::parse("W1AW-10");
    f.src = *Address::parse("N0CALL");
    f.path = *parsePath("RELAY,WIDE2-1");
    f.cr = Cr::Command;
    f.control = 0x00;  // I frame, N(S)=0 N(R)=0
    f.pid = kPidNoLayer3;
    f.info = Bytes{0x01, 0xC0, 0x00, 'h', 'i'};
    return f;
}

}  // namespace

TEST(ax25_address_parse) {
    auto a = Address::parse("n0call-15");
    REQUIRE(a);
    CHECK_EQ(a->call, std::string("N0CALL"));
    CHECK_EQ(a->ssid, 15);
    CHECK(!Address::parse("N0CALL-16"));
    CHECK(!Address::parse("TOOLONGX"));
    CHECK(!Address::parse(""));
    auto star = Address::parse("WIDE1-1*");
    REQUIRE(star);
    CHECK(star->hBit);
    CHECK_EQ(star->str(), std::string("WIDE1-1*"));
}

TEST(ax25_command_bits_roundtrip) {
    Frame f = iFrame();
    auto bytes = encode(f);
    REQUIRE(bytes);
    // Destination SSID byte carries C=1, source C=0 for a command.
    CHECK(((*bytes)[6] & 0x80) != 0);
    CHECK(((*bytes)[13] & 0x80) == 0);
    auto back = decode(*bytes);
    REQUIRE(back);
    CHECK(back->cr == Cr::Command);
    CHECK_EQ(back->info, f.info);
    CHECK_EQ(back->control, 0x00);
    CHECK(back->pid.has_value());
    CHECK_EQ(back->path.size(), 2u);

    f.cr = Cr::Response;
    auto resp = decode(*encode(f));
    REQUIRE(resp);
    CHECK(resp->cr == Cr::Response);
}

TEST(ax25_supervisory_frames_have_no_pid) {
    Frame f = iFrame();
    f.control = 0x21;  // RR, N(R)=1
    f.pid.reset();
    f.info.clear();
    auto bytes = encode(f);
    REQUIRE(bytes);
    CHECK_EQ(bytes->size(), 7u * 4 + 1);
    auto back = decode(*bytes);
    REQUIRE(back);
    CHECK(!back->pid.has_value());
    CHECK_EQ(back->control, 0x21);

    f.pid = kPidNoLayer3;  // a PID on an S frame is a caller bug
    CHECK(!encode(f));
}

TEST(ax25_fully_repeated_needs_last_digi_h_bit) {
    Frame f = iFrame();
    CHECK(!f.fullyRepeated());
    f.path[0].hBit = true;
    CHECK(f.wasDigipeated());
    CHECK(!f.fullyRepeated());
    f.path[1].hBit = true;
    CHECK(f.fullyRepeated());
    auto back = decode(*encode(f));
    REQUIRE(back);
    CHECK(back->fullyRepeated());
    f.path.clear();
    CHECK(f.fullyRepeated());
}

TEST(ax25_rejects_bad_frames) {
    CHECK(!decode(Bytes{}));
    CHECK(!decode(Bytes(13, 0x40)));
    Bytes noControl = *encode(iFrame());
    noControl.resize(7 * 4);
    CHECK(!decode(noControl));
    Bytes noPid = *encode(iFrame());
    noPid.resize(7 * 4 + 1);  // I frame without its PID
    CHECK(!decode(noPid));
}

TEST(ax25_parse_path_allows_spaces_and_empty) {
    auto p = parsePath("RELAY, WIDE2-1");
    REQUIRE(p);
    CHECK_EQ(p->size(), 2u);
    auto e = parsePath("");
    REQUIRE(e);
    CHECK(e->empty());
    CHECK(!parsePath("A,B,C,D,E,F,G,H,I"));
}

// Real frames captured from a TH-D75 during the aprs_sms bench check.
TEST(ax25_decodes_bench_captures) {
    std::ifstream in(std::string(OGM_FIXTURE_DIR) + "/bench.kisslog");
    REQUIRE(in);
    int frames = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto sp = line.find(' ');
        REQUIRE(sp != std::string::npos);
        Bytes kissBody = fromHex(line.substr(sp + 1));
        REQUIRE(!kissBody.empty());
        if (kissBody[0] != 0x00) continue;
        std::string err;
        auto f = decode(std::span(kissBody).subspan(1), &err);
        if (!f) {
            ::t::fail(__FILE__, __LINE__, err + ": " + line);
            continue;
        }
        CHECK_EQ(f->control, kControlUi);
        // Re-encoding gives back the same bytes, C bits included.
        auto again = encode(*f);
        REQUIRE(again);
        CHECK(std::equal(again->begin(), again->end(), kissBody.begin() + 1, kissBody.end()));
        ++frames;
    }
    CHECK(frames > 10);
}
