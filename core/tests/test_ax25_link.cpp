#include <functional>
#include <string>

#include "test.hpp"
#include "ogm/ax25_link.hpp"
#include "ogm/b2f.hpp"
#include "ogm/message.hpp"

using ogm::Bytes;
using ogm::TimeMs;
using namespace ogm::ax25;
using Kind = Link::Event::Kind;

namespace {

// Two stations on one simplex channel.  Frames arrive after a fixed delay
// (air time is folded in); `drop` decides which frames get lost, and an
// optional digipeater in the middle repeats frames and sets their H bit.
struct Air {
    struct InFlight {
        TimeMs at;
        int to;  // 0 or 1
        Frame f;
    };
    Air(Link* a, Link* b) : st{a, b} {}
    Link* st[2];
    std::vector<InFlight> flying;
    std::function<bool(int from, const Frame&)> drop = [](int, const Frame&) { return false; };
    std::vector<Link::Event> ev[2];
    Bytes rx[2];
    TimeMs now = 0;
    TimeMs delay = 700;
    bool digi = false;
    int sent[2] = {0, 0};

    void step(TimeMs dt) {
        now += dt;
        for (int i = 0; i < 2; ++i) {
            st[i]->tick(now);
            for (auto& f : st[i]->takeOutgoing()) {
                ++sent[i];
                if (drop(i, f)) continue;
                if (digi) {
                    // The listener also hears the original, before the digi: it must ignore it.
                    flying.push_back({now + delay / 2, 1 - i, f});
                    for (auto& a : f.path) a.hBit = true;
                }
                flying.push_back({now + delay, 1 - i, f});
            }
        }
        std::vector<InFlight> later;
        for (auto& x : flying) {
            if (x.at <= now) {
                st[x.to]->onFrame(x.f, now);
            } else {
                later.push_back(std::move(x));
            }
        }
        flying = std::move(later);
        for (int i = 0; i < 2; ++i)
            for (auto& e : st[i]->takeEvents()) {
                if (e.kind == Kind::Data) ogm::append(rx[i], e.data);
                ev[i].push_back(std::move(e));
            }
    }
    void run(TimeMs total, TimeMs dt = 50) {
        for (TimeMs t = 0; t < total; t += dt) step(dt);
    }
    bool saw(int i, Kind k) const {
        for (const auto& e : ev[i])
            if (e.kind == k) return true;
        return false;
    }
    std::string reason(int i) const {
        for (const auto& e : ev[i])
            if (e.kind == Kind::Disconnected) return e.text;
        return "(still up)";
    }
};

LinkConfig cfg(const char* call, bool accept = false) {
    LinkConfig c;
    c.mycall = *Address::parse(call);
    c.acceptIncoming = accept;
    return c;
}

Bytes pattern(std::size_t n, std::uint8_t seed) {
    Bytes b(n);
    for (std::size_t i = 0; i < n; ++i) b[i] = static_cast<std::uint8_t>(i * 7 + seed);
    return b;
}

}  // namespace

TEST(link_connect_transfer_disconnect) {
    Link a(cfg("N0CALL")), b(cfg("W1AW-10", true));
    Air air(&a, &b);
    a.connect(*Address::parse("W1AW-10"), {}, 0);
    air.run(3000);
    REQUIRE(a.connected() && b.connected());
    CHECK(air.saw(0, Kind::Connected));
    CHECK(air.saw(1, Kind::Connected));

    Bytes up = pattern(5000, 1), downData = pattern(3000, 2);
    a.write(up, air.now);
    b.write(downData, air.now);
    air.run(120000);
    CHECK(air.rx[1] == up);
    CHECK(air.rx[0] == downData);
    CHECK_EQ(a.queuedBytes(), 0u);
    CHECK_EQ(a.ackedBytes(), 5000u);

    a.disconnect(air.now);
    air.run(10000);
    CHECK(a.state() == Link::State::Disconnected);
    CHECK(b.state() == Link::State::Disconnected);
    CHECK_EQ(air.reason(0), std::string());
    CHECK_EQ(air.reason(1), std::string());
}

TEST(link_survives_heavy_frame_loss) {
    Link a(cfg("N0CALL")), b(cfg("W1AW-10", true));
    Air air(&a, &b);
    int n = 0;
    // Lose one frame in four, in both directions, acks and polls included.
    air.drop = [&](int, const Frame&) { return ++n % 4 == 0; };
    a.connect(*Address::parse("W1AW-10"), {}, 0);
    air.run(20000);
    REQUIRE(a.connected());
    Bytes up = pattern(4000, 3), downData = pattern(2500, 4);
    a.write(up, air.now);
    b.write(downData, air.now);
    air.run(600000);
    CHECK(air.rx[1] == up);
    CHECK(air.rx[0] == downData);
    CHECK(a.connected() && b.connected());
}

TEST(link_burst_loss_recovers_by_polling) {
    Link a(cfg("N0CALL")), b(cfg("W1AW-10", true));
    Air air(&a, &b);
    a.connect(*Address::parse("W1AW-10"), {}, 0);
    air.run(3000);
    REQUIRE(a.connected());
    // A fade: everything from b is lost for 15 seconds.
    TimeMs fadeEnd = air.now + 15000;
    air.drop = [&](int from, const Frame&) { return from == 1 && air.now < fadeEnd; };
    Bytes up = pattern(1500, 5);
    a.write(up, air.now);
    air.run(120000);
    CHECK(air.rx[1] == up);
    CHECK(a.connected());
}

TEST(link_gives_up_when_nobody_answers) {
    LinkConfig c = cfg("N0CALL");
    c.retries = 3;
    Link a(c), b(cfg("W1AW-10", true));
    Air air(&a, &b);
    air.drop = [](int, const Frame&) { return true; };
    a.connect(*Address::parse("W1AW-10"), {}, 0);
    air.run(60000);
    CHECK(a.state() == Link::State::Disconnected);
    CHECK_EQ(air.sent[0], 3);
    CHECK(air.reason(0).find("no answer from W1AW-10 after 3 tries") != std::string::npos);
}

TEST(link_refused_with_dm) {
    Link a(cfg("N0CALL")), b(cfg("W1AW-10", false));  // b takes no calls
    Air air(&a, &b);
    a.connect(*Address::parse("W1AW-10"), {}, 0);
    air.run(5000);
    CHECK(a.state() == Link::State::Disconnected);
    CHECK(air.reason(0).find("refused") != std::string::npos);
}

TEST(link_through_a_digipeater) {
    Link a(cfg("N0CALL")), b(cfg("W1AW-10", true));
    Air air(&a, &b);
    air.digi = true;
    a.connect(*Address::parse("W1AW-10"), {*Address::parse("RELAY")}, 0);
    air.run(5000);
    REQUIRE(a.connected() && b.connected());
    // b learned the reverse path from the SABM.
    Bytes up = pattern(900, 6), downData = pattern(700, 7);
    a.write(up, air.now);
    b.write(downData, air.now);
    air.run(120000);
    CHECK(air.rx[1] == up);  // no duplicates from hearing frames before the digi
    CHECK(air.rx[0] == downData);
}

TEST(link_t1_waits_for_estimated_air_time) {
    LinkConfig c = cfg("N0CALL");
    c.frack = 3000;
    Link a(c);
    a.connect(*Address::parse("W1AW-10"), {}, 0);
    a.takeOutgoing();
    // SABM: 17 bytes + overhead at 1200 baud is ~150 ms; plus 300 ms TXDELAY.
    auto d = a.nextDeadline();
    REQUIRE(d);
    CHECK(*d > 3300 && *d < 3600);
}

TEST(link_ignores_other_stations_traffic) {
    Link a(cfg("N0CALL"));
    a.connect(*Address::parse("W1AW-10"), {}, 0);
    a.takeOutgoing();
    Frame ua;
    ua.dest = *Address::parse("K1ABC");
    ua.src = *Address::parse("W1AW-10");
    ua.cr = Cr::Response;
    ua.control = 0x73;
    ua.pid.reset();
    a.onFrame(ua, 100);
    CHECK(a.state() == Link::State::Connecting);
    ua.dest = *Address::parse("N0CALL");
    a.onFrame(ua, 200);
    CHECK(a.connected());
}

// The whole stack: B2F client over AX.25 over a lossy channel to a B2F
// master standing in for an RMS gateway.
TEST(link_carries_a_full_winlink_session) {
    Link a(cfg("N0CALL")), b(cfg("W1AW-10", true));
    Air air(&a, &b);
    int n = 0;
    air.drop = [&](int, const Frame&) { return ++n % 7 == 0; };

    ogm::Message::Draft d;
    d.from = "N0CALL";
    d.to = {"friend@example.com"};
    d.subject = "Over packet";
    d.body = std::string(1200, 'p') + "\n";
    auto m = ogm::Message::compose(d, 1795037100, 1);
    ogm::b2f::Config cc;
    cc.mycall = "N0CALL";
    cc.targetcall = "W1AW-10";
    cc.password = "pw";
    ogm::b2f::Config mc;
    mc.mycall = "W1AW-10";
    mc.master = true;
    mc.challenge = "87654321";
    mc.password = "pw";
    ogm::b2f::Session client(cc, {{m.mid(), m.header("Subject"), m.serialize()}}, nullptr);
    ogm::b2f::Session master(mc, {}, nullptr);

    a.connect(*Address::parse("W1AW-10"), {}, 0);
    bool masterStarted = false;
    std::size_t rxA = 0, rxB = 0;
    bool delivered = false;
    for (int i = 0; i < 20000 && !(client.finished() && master.finished() && a.state() == Link::State::Disconnected); ++i) {
        air.step(50);
        if (b.connected() && !masterStarted) {
            master.start();
            masterStarted = true;
        }
        if (air.rx[0].size() > rxA) {
            client.onData(std::span(air.rx[0]).subspan(rxA));
            rxA = air.rx[0].size();
        }
        if (air.rx[1].size() > rxB) {
            master.onData(std::span(air.rx[1]).subspan(rxB));
            rxB = air.rx[1].size();
        }
        if (auto o = client.takeOut(); !o.empty()) a.write(o, air.now);
        if (auto o = master.takeOut(); !o.empty()) b.write(o, air.now);
        for (auto& e : master.takeEvents())
            if (e.kind == ogm::b2f::Event::Kind::Received && e.data == m.serialize()) delivered = true;
        client.takeEvents();
        if (client.finished() && a.connected()) a.disconnect(air.now);
    }
    CHECK(delivered);
    CHECK(client.finished() && !client.failed());
    CHECK(master.finished() && !master.failed());
    CHECK(a.state() == Link::State::Disconnected);
}

// Found by fuzzing: a SABM from a "callsign" of garbage was accepted.
TEST(link_ignores_frames_from_invalid_callsigns) {
    LinkConfig c = cfg("W1AW-10", true);
    Link b(c);
    Frame sabm;
    sabm.dest = *Address::parse("W1AW-10");
    sabm.src = Address{"n0~ll", 0, false};  // lower case and '~': not a callsign
    sabm.cr = Cr::Command;
    sabm.control = 0x3F;
    sabm.pid.reset();
    b.onFrame(sabm, 0);
    CHECK(b.state() == Link::State::Disconnected);
    CHECK(b.takeOutgoing().empty());
}
