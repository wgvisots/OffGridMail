#include <filesystem>
#include <functional>
#include <random>
#include <string>

#include "../../sim/fake_rms.hpp"
#include "test.hpp"
#include "ogm/engine.hpp"
#include "ogm/kiss.hpp"
#include "ogm/radio.hpp"

using namespace ogm;
namespace fs = std::filesystem;
using EKind = Engine::Event::Kind;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("ogm-test-" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

Message::Draft draft(const std::string& from, const std::string& to, const std::string& subject) {
    Message::Draft d;
    d.from = from;
    d.to = {to};
    d.subject = subject;
    d.body = "Test message: " + subject + "\n";
    return d;
}

// Moves bytes between the engine (as the shell would) and the fake gateway.
// `radio` (optional) sits in between and can rewrite either direction.
struct Harness {
    Harness(Engine& e, sim::FakeRms& r) : engine(e), rms(r) {}
    Engine& engine;
    sim::FakeRms& rms;
    TimeMs now = 0;
    std::vector<Engine::Event> events;
    std::function<Bytes(ByteView)> toRms = [](ByteView b) { return Bytes(b.begin(), b.end()); };
    std::function<Bytes(ByteView)> toEngine = [](ByteView b) { return Bytes(b.begin(), b.end()); };

    void connect() {
        rms.onClientConnected(now);
        engine.onLowerUp(now);
    }
    // Runs until the engine closes its socket (or time runs out).
    void run(TimeMs limit = 600000) {
        bool down = false;
        while (now < limit) {
            now += 50;
            engine.tick(now);
            rms.tick(now);
            if (auto b = engine.takeLowerOut(); !b.empty()) {
                auto x = toRms(b);
                if (!x.empty()) rms.onBytes(x, now);
            }
            if (auto b = rms.takeOut(); !b.empty()) {
                auto x = toEngine(b);
                if (!x.empty()) engine.onLowerBytes(x, now);
            }
            rms.takeLog();
            for (auto& e : engine.takeEvents()) events.push_back(std::move(e));
            if (!down && rms.wantsHangup()) {
                down = true;
                engine.onLowerDown(now);
            }
            if (engine.lowerDone() && !engine.sessionActive()) {
                // Like a real shell: write what is left, then close.
                if (auto b = engine.takeLowerOut(); !b.empty()) toRms(b);
                break;
            }
        }
        for (auto& e : engine.takeEvents()) events.push_back(std::move(e));
    }
    const Engine::Event* ended() const {
        for (const auto& e : events)
            if (e.kind == EKind::SessionEnded) return &e;
        return nullptr;
    }
};

std::unique_ptr<Channel> ax25Channel(std::unique_ptr<radio::KissTnc> tnc) {
    Ax25Channel::Config c;
    c.link.mycall = *ax25::Address::parse("N0CALL");
    c.target = *ax25::Address::parse("W1AW-10");
    return std::make_unique<Ax25Channel>(c, std::move(tnc));
}

}  // namespace

TEST(mailbox_basics) {
    TempDir dir;
    Mailbox mb(dir.path);
    CHECK(fs::is_directory(dir.path / "in"));
    CHECK(mb.save(Mailbox::Folder::Out, "ABC123", asBytes("hello")));
    CHECK(mb.has("ABC123"));
    CHECK(mb.find("ABC123") == Mailbox::Folder::Out);
    CHECK(mb.move("ABC123", Mailbox::Folder::Out, Mailbox::Folder::Sent));
    CHECK(mb.list(Mailbox::Folder::Out).empty());
    REQUIRE(mb.list(Mailbox::Folder::Sent).size() == 1);
    auto raw = mb.load(Mailbox::Folder::Sent, "ABC123");
    REQUIRE(raw);
    CHECK_EQ(std::string(raw->begin(), raw->end()), std::string("hello"));
    // Real CMS message IDs carry '#' and '$'.
    CHECK(mb.save(Mailbox::Folder::In, "NZ5R#$29RI1R", asBytes("from the CMS")));
    CHECK(mb.find("NZ5R#$29RI1R") == Mailbox::Folder::In);
    REQUIRE(mb.list(Mailbox::Folder::In).size() == 1);
    CHECK_EQ(mb.list(Mailbox::Folder::In)[0], std::string("NZ5R#$29RI1R"));
    // MIDs come off the air: no path tricks.
    CHECK(!mb.save(Mailbox::Folder::In, "../evil", asBytes("x")));
    CHECK(!mb.save(Mailbox::Folder::In, "a/b", asBytes("x")));
    CHECK(!mb.save(Mailbox::Folder::In, ".hidden", asBytes("x")));
    CHECK(!mb.save(Mailbox::Folder::In, "sp ace", asBytes("x")));
    auto r = mb.rescue("a/b#c", asBytes("x"));
    CHECK(!r.empty() && fs::exists(r) && r.parent_path().filename() == "rescue");
    CHECK(!mb.save(Mailbox::Folder::In, "", asBytes("x")));
    CHECK(!mb.has("../in/ABC123"));
}

TEST(th_d75_bring_up) {
    radio::ThD75 d75;
    d75.onLinkUp(0);
    Bytes out = d75.takeOut();
    std::string s(out.begin(), out.end());
    CHECK_EQ(s, std::string("\xC0\xFF\xC0\rID\r"));
    d75.onText("?\r", 10);
    d75.onText("ID TH-D75\r", 20);
    out = d75.takeOut();
    CHECK_EQ(std::string(out.begin(), out.end()), std::string("TN 2,0\r"));
    CHECK_EQ(d75.radioId(), std::string("TH-D75"));
    d75.onText("TN 2,0\r", 30);
    out = d75.takeOut();
    CHECK_EQ(std::string(out.begin(), out.end()), std::string("TN\r"));
    CHECK(d75.state() == radio::KissTnc::State::Starting);
    d75.tick(1000);
    CHECK(d75.state() == radio::KissTnc::State::Starting);
    d75.tick(2100);  // TN went unanswered: KISS is on
    CHECK(d75.state() == radio::KissTnc::State::Ready);
    CHECK(d75.shutdownBytes() == kiss::exitSequence());
}

TEST(th_d75_tries_the_other_band_then_gives_up) {
    radio::ThD75 d75;
    d75.onLinkUp(0);
    d75.onText("ID TH-D75", 1);
    d75.takeOut();
    d75.onText("N", 2);
    Bytes out = d75.takeOut();
    CHECK_EQ(std::string(out.begin(), out.end()), std::string("TN 2,1\r"));
    d75.onText("N", 3);
    CHECK(d75.state() == radio::KissTnc::State::Failed);
}

TEST(th_d75_silent_radio_fails_with_a_hint) {
    radio::ThD75 d75;
    d75.onLinkUp(0);
    d75.tick(2500);
    CHECK(d75.state() == radio::KissTnc::State::Failed);
    CHECK(d75.status().find("does not answer") != std::string::npos);
}

TEST(engine_telnet_session_both_ways) {
    TempDir dir;
    Mailbox mine(dir.path / "me"), theirs(dir.path / "rms");
    Engine engine({"n0call", "pw", "EM73"}, mine);
    std::string err;
    std::string mid = engine.compose(draft("", "friend@example.com", "Hello CMS"), 1795037100, &err);
    REQUIRE(!mid.empty());
    auto incoming = Message::compose(draft("W1AW", "N0CALL", "Hi back"), 1795037000, 99);
    theirs.save(Mailbox::Folder::Out, incoming.mid(), incoming.serialize());

    sim::FakeRms::Config rc;
    rc.telnet = true;
    rc.password = "pw";
    sim::FakeRms rms(rc, theirs);
    REQUIRE(engine.startSession(std::make_unique<TelnetChannel>(TelnetChannel::Config{"N0CALL"}), 0));
    Harness h(engine, rms);
    h.connect();
    h.run();

    auto* end = h.ended();
    REQUIRE(end);
    CHECK(end->ok);
    CHECK_EQ(end->text, std::string("done: 1 sent, 1 received"));
    CHECK(mine.list(Mailbox::Folder::Out).empty());
    CHECK(mine.find(mid) == Mailbox::Folder::Sent);
    CHECK(mine.find(incoming.mid()) == Mailbox::Folder::In);
    CHECK(theirs.find(mid) == Mailbox::Folder::In);
    CHECK(!engine.sessionActive());
}

TEST(engine_ax25_session_with_frame_loss) {
    TempDir dir;
    Mailbox mine(dir.path / "me"), theirs(dir.path / "rms");
    Engine engine({"N0CALL", "pw", ""}, mine);
    Message::Draft big = draft("", "W1AW", "Bigger one");
    big.body = std::string(4000, 'b');
    big.files = {{"data.bin", Bytes(1500, 0x55)}};
    std::string mid = engine.compose(big, 1795037100);
    REQUIRE(!mid.empty());
    auto incoming = Message::compose(draft("W1AW", "N0CALL", "Reply"), 1795037000, 7);
    theirs.save(Mailbox::Folder::Out, incoming.mid(), incoming.serialize());

    sim::FakeRms::Config rc;
    rc.password = "pw";
    rc.dropEvery = 5;
    sim::FakeRms rms(rc, theirs);
    REQUIRE(engine.startSession(ax25Channel(std::make_unique<radio::PlainKiss>()), 0));
    Harness h(engine, rms);
    h.connect();
    h.run();

    auto* end = h.ended();
    REQUIRE(end);
    if (!end->ok) ::t::fail(__FILE__, __LINE__, "session failed: " + end->text);
    CHECK(mine.find(mid) == Mailbox::Folder::Sent);
    CHECK(mine.find(incoming.mid()) == Mailbox::Folder::In);
    auto got = theirs.load(Mailbox::Folder::In, mid);
    REQUIRE(got);
    auto m = Message::parse(*got);
    REQUIRE(m);
    CHECK_EQ(m->files().size(), 1u);
}

// The whole D75 path: probe, KISS on, AX.25, B2F, and KISS off at the end.
TEST(engine_through_a_simulated_th_d75) {
    TempDir dir;
    Mailbox mine(dir.path / "me"), theirs(dir.path / "rms");
    Engine engine({"N0CALL", "", ""}, mine);
    std::string mid = engine.compose(draft("", "W1AW", "Via the D75"), 1795037100);
    sim::FakeRms rms({}, theirs);

    // The radio: answers PC commands until "TN 2,x", then passes KISS through.
    bool kissOn = false, exitedKiss = false;
    std::string cmd;
    Bytes radioReplies;
    REQUIRE(engine.startSession(ax25Channel(std::make_unique<radio::ThD75>()), 0));
    Harness h(engine, rms);
    h.toRms = [&](ByteView b) {
        Bytes pass;
        for (std::size_t i = 0; i < b.size(); ++i) {
            if (b.size() - i >= 3 && b[i] == 0xC0 && b[i + 1] == 0xFF && b[i + 2] == 0xC0) {
                if (kissOn) exitedKiss = true;
                kissOn = false;
                i += 2;
                continue;
            }
            if (kissOn) {
                pass.push_back(b[i]);
                continue;
            }
            char c = static_cast<char>(b[i]);
            if (c != '\r') {
                cmd += c;
                continue;
            }
            if (cmd == "ID") append(radioReplies, std::string_view("ID TH-D75\r"));
            else if (cmd.rfind("TN 2,", 0) == 0) {
                append(radioReplies, cmd + "\r");
                kissOn = true;
            } else if (!cmd.empty()) append(radioReplies, std::string_view("?\r"));
            cmd.clear();
        }
        return pass;
    };
    h.toEngine = [&](ByteView b) {
        Bytes all = std::exchange(radioReplies, {});
        append(all, b);
        return all;
    };
    h.connect();
    // Replies to the probe come back on the next ticks.
    for (int i = 0; i < 5; ++i) {
        h.now += 50;
        engine.onLowerBytes(std::exchange(radioReplies, {}), h.now);
        if (auto b = engine.takeLowerOut(); !b.empty()) h.toRms(b);
    }
    h.run();
    auto* end = h.ended();
    REQUIRE(end);
    if (!end->ok) ::t::fail(__FILE__, __LINE__, "session failed: " + end->text);
    CHECK(mine.find(mid) == Mailbox::Folder::Sent);
    CHECK(exitedKiss);  // the radio is back to normal
}

TEST(engine_reports_a_wrong_password) {
    TempDir dir;
    Mailbox mine(dir.path / "me"), theirs(dir.path / "rms");
    Engine engine({"N0CALL", "wrong", ""}, mine);
    engine.compose(draft("", "W1AW", "Will not go"), 1795037100);
    sim::FakeRms::Config rc;
    rc.telnet = true;
    rc.password = "right";
    sim::FakeRms rms(rc, theirs);
    engine.startSession(std::make_unique<TelnetChannel>(TelnetChannel::Config{"N0CALL"}), 0);
    Harness h(engine, rms);
    h.connect();
    h.run();
    auto* end = h.ended();
    REQUIRE(end);
    CHECK(!end->ok);
    CHECK(end->text.find("Secure login failed") != std::string::npos);
    CHECK_EQ(mine.list(Mailbox::Folder::Out).size(), 1u);  // still waiting
}

TEST(engine_reports_no_answer) {
    TempDir dir;
    Mailbox mine(dir.path / "me"), theirs(dir.path / "rms");
    Engine engine({"N0CALL", "", ""}, mine);
    sim::FakeRms rms({}, theirs);
    engine.startSession(ax25Channel(std::make_unique<radio::PlainKiss>()), 0);
    Harness h(engine, rms);
    h.toRms = [](ByteView) { return Bytes{}; };  // nobody hears us
    h.connect();
    h.run();
    auto* end = h.ended();
    REQUIRE(end);
    CHECK(!end->ok);
    CHECK(end->text.find("no answer from W1AW-10") != std::string::npos);
}

TEST(engine_compose_rejects_invalid) {
    TempDir dir;
    Mailbox mine(dir.path);
    Engine engine({"N0CALL", "", ""}, mine);
    std::string err;
    Message::Draft d = draft("", "W1AW", "");
    CHECK(engine.compose(d, 0, &err).empty());
    CHECK_EQ(err, std::string("empty subject"));
    CHECK(mine.list(Mailbox::Folder::Out).empty());
}
