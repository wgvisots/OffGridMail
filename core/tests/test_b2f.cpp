#include <fstream>
#include <iterator>
#include <string>

#include "test.hpp"
#include "ogm/b2f.hpp"
#include "ogm/lzhuf.hpp"
#include "ogm/message.hpp"

using ogm::Bytes;
using namespace ogm::b2f;
using Kind = Event::Kind;

namespace {

std::string text(const Bytes& b) { return std::string(b.begin(), b.end()); }

Config clientCfg() {
    Config c;
    c.mycall = "N0CALL";
    c.targetcall = "WL2K";
    c.locator = "EM73";
    return c;
}

Outgoing makeOutgoing(const std::string& to, const std::string& subject, const std::string& body, std::uint64_t salt) {
    ogm::Message::Draft d;
    d.from = "N0CALL";
    d.to = {to};
    d.subject = subject;
    d.body = body;
    auto m = ogm::Message::compose(d, 1795037100, salt);
    return {m.mid(), m.header("Subject"), m.serialize()};
}

bool has(const std::vector<Event>& ev, Kind k, const std::string& mid = {}) {
    for (const auto& e : ev)
        if (e.kind == k && (mid.empty() || e.mid == mid)) return true;
    return false;
}

std::string failure(const std::vector<Event>& ev) {
    for (const auto& e : ev)
        if (e.kind == Kind::Failed) return e.text;
    return {};
}

// Run two sessions against each other until neither has anything to say.
void pump(Session& a, Session& b, std::vector<Event>& evA, std::vector<Event>& evB) {
    for (int i = 0; i < 1000; ++i) {
        Bytes ab = a.takeOut(), ba = b.takeOut();
        if (ab.empty() && ba.empty()) break;
        if (!ab.empty()) b.onData(ab);
        if (!ba.empty()) a.onData(ba);
        for (auto& e : a.takeEvents()) evA.push_back(std::move(e));
        for (auto& e : b.takeEvents()) evB.push_back(std::move(e));
    }
    for (auto& e : a.takeEvents()) evA.push_back(std::move(e));
    for (auto& e : b.takeEvents()) evB.push_back(std::move(e));
}

}  // namespace

// Same exchange as Pat's TestSessionCMS.
TEST(b2f_client_with_nothing_to_send) {
    Session s(clientCfg(), {}, nullptr);
    s.start();
    CHECK(s.takeOut().empty());
    s.onData(ogm::asBytes("[WL2K-5.0-B2FWIHJM$]\rFoobar should be ignored\rTest CMS >\r"));
    CHECK_EQ(text(s.takeOut()), std::string(";FW: N0CALL\r[OffGridMail-0.1-B2FHM$]\r; WL2K DE N0CALL (EM73)\rFF\r"));
    CHECK(!s.finished());
    s.onData(ogm::asBytes("FQ\r"));
    CHECK(s.finished());
    CHECK(!s.failed());
    CHECK(s.quitting());
}

TEST(b2f_secure_login_answer) {
    Config c = clientCfg();
    c.password = "FOOBAR";
    Session s(c, {}, nullptr);
    s.onData(ogm::asBytes("[WL2K-5.0-B2FWIHJM$]\r\n;PQ: 23753528\r\nCMS>\r\n"));
    std::string out = text(s.takeOut());
    CHECK(out.find(";PR: 72768415\r") != std::string::npos);
    // The response comes after the SID, before the comment line.
    CHECK(out.find("[OffGridMail-") < out.find(";PR:"));
}

// A new callsign has no password yet: answer without ;PR and let the CMS decide.
TEST(b2f_challenge_without_password_continues) {
    Session s(clientCfg(), {}, nullptr);
    s.onData(ogm::asBytes("[WL2K-5.0-B2FWIHJM$]\r;PQ: 23753528\rCMS>\r"));
    CHECK(!s.failed());
    std::string out = text(s.takeOut());
    CHECK(out.find(";PR:") == std::string::npos);
    CHECK(out.find("FF\r") != std::string::npos);
}

TEST(b2f_gateway_error_line_fails_session) {
    Session s(clientCfg(), {}, nullptr);
    s.onData(ogm::asBytes("[WL2K-5.0-B2FWIHJM$]\rCMS>\r"));
    s.takeOut();
    s.onData(ogm::asBytes("*** Secure login failed - account password does not match. - Disconnecting\r"));
    CHECK(s.failed());
    CHECK_EQ(failure(s.takeEvents()), std::string("Secure login failed - account password does not match. - Disconnecting"));
}

TEST(b2f_rejects_non_b2_remote) {
    Session s(clientCfg(), {}, nullptr);
    s.onData(ogm::asBytes("[FBB-7.00-AB1FHMRX$]\r"));
    CHECK(s.failed());
}

TEST(b2f_proposal_checksum) {
    // Pat's TestSessionCMSWithMessage block: one proposal, checksum by hand.
    std::string l = "FC EM TJKYEIMMHSRB 527 123 0";
    unsigned sum = '\r';
    for (char c : l) sum += static_cast<unsigned char>(c);
    CHECK_EQ(proposalChecksum({l}), (0x100 - (sum & 0xff)) & 0xff);
}

// The gateway offers one real Winlink message (31 KB with a JPEG attachment).
TEST(b2f_client_downloads_a_message) {
    std::ifstream in(std::string(OGM_FIXTURE_DIR) + "/lzhuf/LPE5NXDVLVSQ.b2f", std::ios::binary);
    Bytes raw(std::istreambuf_iterator<char>(in), {});
    REQUIRE(!raw.empty());
    Bytes packed = ogm::lzhuf::compress(raw);

    Session s(clientCfg(), {}, nullptr);
    s.onData(ogm::asBytes("[WL2K-5.0-B2FWIHJM$]\rCMS>\r"));
    CHECK(text(s.takeOut()).find("FF\r") != std::string::npos);

    std::string prop = "FC EM LPE5NXDVLVSQ " + std::to_string(raw.size()) + " " + std::to_string(packed.size()) + " 0";
    char sum[8];
    std::snprintf(sum, sizeof sum, "%02X", proposalChecksum({prop}));
    s.onData(ogm::asBytes(";PM: N0CALL LPE5NXDVLVSQ 31380 LA5NTA 73 fra Brekke\r" + prop + "\rF> " + sum + "\r"));
    CHECK_EQ(text(s.takeOut()), std::string("FS +\r"));
    REQUIRE(s.pending().count("LPE5NXDVLVSQ") == 1);
    CHECK_EQ(s.pending().at("LPE5NXDVLVSQ").subject, std::string("73 fra Brekke"));

    // The binary transfer, in 250-byte blocks (what the CMS uses on Telnet).
    Bytes wire{0x01};
    std::string title = "73 fra Brekke";
    wire.push_back(static_cast<std::uint8_t>(title.size() + 3));
    ogm::append(wire, title);
    wire.push_back(0);
    wire.push_back('0');
    wire.push_back(0);
    unsigned csum = 0;
    for (std::size_t i = 0; i < packed.size(); i += 250) {
        std::size_t n = std::min<std::size_t>(250, packed.size() - i);
        wire.push_back(0x02);
        wire.push_back(static_cast<std::uint8_t>(n));
        for (std::size_t k = 0; k < n; ++k) {
            wire.push_back(packed[i + k]);
            csum += packed[i + k];
        }
    }
    wire.push_back(0x04);
    wire.push_back(static_cast<std::uint8_t>((0x100 - (csum & 0xff)) & 0xff));
    // Deliver it in awkward pieces.
    for (std::size_t i = 0; i < wire.size(); i += 77)
        s.onData(std::span(wire).subspan(i, std::min<std::size_t>(77, wire.size() - i)));

    auto ev = s.takeEvents();
    REQUIRE(has(ev, Kind::Received, "LPE5NXDVLVSQ"));
    for (const auto& e : ev)
        if (e.kind == Kind::Received) CHECK(e.data == raw);
    // After receiving, it's our turn again: nothing to send, and the remote
    // sent messages, so FF (not FQ).
    CHECK_EQ(text(s.takeOut()), std::string("FF\r"));
    s.onData(ogm::asBytes("FQ\r"));
    CHECK(s.finished() && !s.failed());
}

TEST(b2f_bad_proposal_checksum_fails) {
    Session s(clientCfg(), {}, nullptr);
    s.onData(ogm::asBytes("[WL2K-5.0-B2FWIHJM$]\rCMS>\r"));
    s.takeOut();
    s.onData(ogm::asBytes("FC EM ABCDEFGHIJKL 100 50 0\rF> 00\r"));
    CHECK(s.failed());
}

TEST(b2f_client_and_master_exchange_both_ways) {
    Config cc = clientCfg();
    cc.password = "secret";
    Config mc;
    mc.mycall = "W1AW-10";
    mc.master = true;
    mc.challenge = "12345678";
    mc.password = "secret";

    auto a = makeOutgoing("friend@example.com", "First", "Hello from the field.\n", 1);
    auto b = makeOutgoing("W1AW", "Second", std::string(3000, 'x') + "\n", 2);
    auto c = makeOutgoing("N0CALL", "For you", "A reply.\n", 3);

    Session client(cc, {a, b}, nullptr);
    Session master(mc, {c}, [&](const std::string& mid) { return mid == "ALREADYHAVE"; });
    master.start();
    std::vector<Event> evC, evM;
    pump(client, master, evC, evM);

    CHECK_EQ(failure(evC), std::string());
    CHECK_EQ(failure(evM), std::string());
    CHECK(client.finished() && !client.failed());
    CHECK(master.finished() && !master.failed());
    CHECK(has(evC, Kind::Sent, a.mid));
    CHECK(has(evC, Kind::Sent, b.mid));
    CHECK(has(evC, Kind::Received, c.mid));
    CHECK(has(evM, Kind::Received, a.mid));
    CHECK(has(evM, Kind::Received, b.mid));
    for (const auto& e : evM)
        if (e.kind == Kind::Received && e.mid == b.mid) CHECK(e.data == b.raw);
}

TEST(b2f_master_refuses_bad_password) {
    Config cc = clientCfg();
    cc.password = "wrong";
    Config mc;
    mc.mycall = "W1AW-10";
    mc.master = true;
    mc.challenge = "12345678";
    mc.password = "secret";
    Session client(cc, {}, nullptr);
    Session master(mc, {}, nullptr);
    master.start();
    std::vector<Event> evC, evM;
    pump(client, master, evC, evM);
    CHECK(master.failed());
    CHECK(client.failed());
    CHECK(failure(evC).find("Secure login failed") != std::string::npos);
}

TEST(b2f_already_received_message_is_refused) {
    auto c = makeOutgoing("N0CALL", "Dup", "Again.\n", 9);
    Config mc;
    mc.mycall = "W1AW-10";
    mc.master = true;
    Session client(clientCfg(), {}, [&](const std::string& mid) { return mid == c.mid; });
    Session master(mc, {c}, nullptr);
    master.start();
    std::vector<Event> evC, evM;
    pump(client, master, evC, evM);
    CHECK(!has(evC, Kind::Received));
    CHECK(has(evM, Kind::Rejected, c.mid));
    CHECK(client.finished() && !client.failed());
}

// If the message can't be stored, the session must stop *before* its next
// turn (FF/FQ/proposals), because that turn is what tells the CMS "delivered".
TEST(b2f_store_failure_stops_before_acknowledging) {
    auto c = makeOutgoing("N0CALL", "Keep me", "Important.\n", 11);
    Config mc;
    mc.mycall = "W1AW-10";
    mc.master = true;
    Session client(clientCfg(), {}, nullptr);
    client.setStore([](const std::string&, const Bytes&, std::string& why) {
        why = "disk full";
        return false;
    });
    Session master(mc, {c}, nullptr);
    master.start();
    std::vector<Event> evC, evM;
    pump(client, master, evC, evM);
    CHECK(client.failed());
    CHECK(!has(evC, Kind::Received));
    CHECK(master.failed());                   // it heard "*** disk full"
    CHECK(!has(evM, Kind::Sent, c.mid));      // and so did not count it as delivered
    CHECK(failure(evM).find("disk full") != std::string::npos);
}
