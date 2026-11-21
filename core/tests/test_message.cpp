#include <fstream>
#include <iterator>
#include <string>

#include "test.hpp"
#include "ogm/message.hpp"

using ogm::Bytes;
using ogm::Message;

namespace {
Bytes readFile(const std::string& name) {
    std::ifstream in(std::string(OGM_FIXTURE_DIR) + "/" + name, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), {});
}
}  // namespace

TEST(message_parses_real_winlink_message) {
    Bytes raw = readFile("lzhuf/LPE5NXDVLVSQ.b2f");
    std::string err;
    auto m = Message::parse(raw, &err);
    REQUIRE(m);
    CHECK_EQ(m->mid(), std::string("LPE5NXDVLVSQ"));
    CHECK_EQ(m->from(), std::string("LA5NTA"));
    CHECK_EQ(m->to().size(), 1u);
    CHECK_EQ(m->subject(), std::string("73 fra Brekke"));
    CHECK_EQ(m->rawBody().size(), 104u);
    // ISO-8859-1 body decodes to UTF-8: "prøver meg på å sende".
    CHECK(m->bodyText().find("pr\xC3\xB8ver meg p\xC3\xA5 \xC3\xA5 sende") != std::string::npos);
    REQUIRE(m->files().size() == 1);
    CHECK_EQ(m->files()[0].name, std::string("1469042410710.jpg"));
    CHECK_EQ(m->files()[0].data.size(), 31028u);
    CHECK_EQ(m->files()[0].data[0], 0xFF);  // JPEG SOI
    CHECK_EQ(m->files()[0].data[1], 0xD8);
    CHECK(m->validate().empty());
}

TEST(message_compose_serialize_parse_roundtrip) {
    Message::Draft d;
    d.from = "n0call";
    d.to = {"friend@example.com", "w1aw@winlink.org"};
    d.cc = {"K1ABC"};
    d.subject = "Caf\xC3\xA9 meeting";  // "Café meeting"
    d.body = "Line one\nLine \xE2\x80\x9Ctwo\xE2\x80\x9D\n";  // curly quotes
    d.files = {{"notes.txt", Bytes{'a', 'b', '\r', '\n'}}};
    Message m = Message::compose(d, 1795037100, 1);  // 2026-11-18 21:25 UTC

    CHECK_EQ(m.mid().size(), 12u);
    CHECK_EQ(m.date(), std::string("2026/11/18 21:25"));
    CHECK_EQ(m.from(), std::string("N0CALL"));
    REQUIRE(m.to().size() == 2);
    CHECK_EQ(m.to()[0], std::string("SMTP:friend@example.com"));
    CHECK_EQ(m.to()[1], std::string("W1AW"));
    CHECK_EQ(m.header("Subject"), std::string("=?ISO-8859-1?Q?Caf=E9_meeting?="));
    CHECK_EQ(m.subject(), std::string("Caf\xC3\xA9 meeting"));
    CHECK_EQ(std::string(m.rawBody().begin(), m.rawBody().end()), std::string("Line one\r\nLine \"two\"\r\n"));
    CHECK(m.validate().empty());

    Bytes wire = m.serialize();
    std::string text(wire.begin(), wire.end());
    CHECK_EQ(text.substr(0, 19), std::string("Mid: " + m.mid() + "\r\n"));
    auto back = Message::parse(wire);
    REQUIRE(back);
    CHECK_EQ(back->mid(), m.mid());
    CHECK_EQ(back->cc().size(), 1u);
    CHECK_EQ(back->subject(), m.subject());
    CHECK(back->rawBody() == m.rawBody());
    REQUIRE(back->files().size() == 1);
    CHECK_EQ(back->files()[0].name, std::string("notes.txt"));
    CHECK(back->files()[0].data == d.files[0].data);
    CHECK(back->serialize() == wire);
}

TEST(message_mids_differ_by_salt_and_time) {
    auto a = ogm::generateMid("N0CALL", 1000, 0);
    auto b = ogm::generateMid("N0CALL", 1000, 1);
    auto c = ogm::generateMid("N0CALL", 1001, 0);
    CHECK(a != b);
    CHECK(a != c);
    CHECK_EQ(a, ogm::generateMid("n0call", 1000, 0));
    for (char ch : a) CHECK((ch >= 'A' && ch <= 'Z') || (ch >= '2' && ch <= '7'));
}

TEST(message_dates) {
    CHECK_EQ(ogm::formatDate(0), std::string("1970/01/01 00:00"));
    CHECK_EQ(ogm::formatDate(951782400), std::string("2000/02/29 00:00"));
    auto t = ogm::parseDate("2026/11/18 21:25");
    REQUIRE(t);
    CHECK_EQ(*t, 1795037100);
    CHECK(ogm::parseDate("2026.11.18 21:25").has_value());
    CHECK(ogm::parseDate("2026-11-18 21:25").has_value());
    CHECK(ogm::parseDate("20261118212500").has_value());
    CHECK(!ogm::parseDate("yesterday"));
    CHECK(!ogm::parseDate("2026/13/01 00:00"));
}

TEST(message_validation) {
    Message::Draft d;
    d.from = "N0CALL";
    d.subject = "x";
    d.body = "y";
    auto m = Message::compose(d, 0);
    CHECK_EQ(m.validate(), std::string("no recipient"));
    d.to = {"W1AW"};
    d.body = "";
    CHECK_EQ(Message::compose(d, 0).validate(), std::string("empty body"));
}

TEST(message_parse_rejects_garbage) {
    std::string err;
    CHECK(!Message::parse(ogm::asBytes("no headers here"), &err));
    CHECK(!Message::parse(ogm::asBytes("Mid: X\r\nBody: 50\r\n\r\nshort"), &err));
    CHECK_EQ(err, std::string("body is shorter than its Body header"));
    CHECK(!Message::parse(ogm::asBytes("Body: 0\r\n\r\n"), &err));
    CHECK_EQ(err, std::string("no Mid header"));
}

TEST(message_charset_helpers) {
    bool changed = false;
    CHECK_EQ(ogm::utf8ToLatin1("plain", &changed), std::string("plain"));
    CHECK(!changed);
    CHECK_EQ(ogm::utf8ToLatin1("\xE2\x80\x94 \xF0\x9F\x98\x80", &changed), std::string("- ?"));
    CHECK(changed);
    CHECK_EQ(ogm::latin1ToUtf8("\xE9"), std::string("\xC3\xA9"));
    CHECK(ogm::isValidUtf8("\xC3\xA9"));
    CHECK(!ogm::isValidUtf8("\xE9t\xE9"));
    CHECK_EQ(ogm::decodeHeaderWords("=?utf-8?B?Q2Fmw6k=?= =?utf-8?Q?_ok?="), std::string("Caf\xC3\xA9 ok"));
    CHECK_EQ(ogm::decodeHeaderWords("Re: =?ISO-8859-1?Q?f=F8r?= now"), std::string("Re: f\xC3\xB8r now"));
    CHECK_EQ(ogm::normalizeAddress(" SMTP:a@b.c "), std::string("SMTP:a@b.c"));
}

// Text of the CMS activation message, as reproduced in Pat's tests.
TEST(message_detects_account_activation) {
    Message::Draft d;
    d.from = "SERVICE";
    d.to = {"N0CALL"};
    d.subject = "Your New Winlink Account";
    d.body = "A new Winlink account for 'N0CALL' has been activated. The next time you connect to a Winlink "
             "server or gateway you will be required to use 'K1CHN7' as your account password (no quotes).\n\n"
             "You can manage your Winlink account (to include changing your password) by logging on to the "
             "Winlink web site at https://www.winlink.org.\n";
    auto pw = ogm::accountActivationPassword(Message::compose(d, 0));
    REQUIRE(pw);
    CHECK_EQ(*pw, std::string("K1CHN7"));

    d.subject = "Something else";
    CHECK(!ogm::accountActivationPassword(Message::compose(d, 0)));
    d.subject = "Your New Winlink Account";
    d.body = "No quoted word here, password elsewhere.";
    auto none = ogm::accountActivationPassword(Message::compose(d, 0));
    REQUIRE(none);
    CHECK(none->empty());
}
