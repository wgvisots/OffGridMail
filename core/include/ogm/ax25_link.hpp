// AX.25 v2.0 connected mode (the data-link layer packet gateways speak),
// for a KISS TNC that only moves frames.
//
// Like everything in the core it does no I/O: feed it decoded frames and the
// time, take the frames it wants sent, read its events.  Modulo-8 sequence
// numbers, SABM/UA/DISC/DM, I-frames with a window of k, RR/RNR/REJ.
//
// T1 (the retry timer) counts from the estimated *end* of our transmission:
// a KISS TNC queues frames and keys up when it can, so time spent waiting in
// the TNC and on the air must not eat into the gateway's time to answer.
#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "ogm/ax25.hpp"
#include "ogm/common.hpp"

namespace ogm::ax25 {

struct LinkConfig {
    Address mycall;
    std::size_t paclen = 128;     // N1: data bytes per I-frame
    std::size_t maxframe = 4;     // k: unacknowledged I-frames in flight (1..7)
    int retries = 10;             // N2
    TimeMs frack = 4000;          // T1 after our transmission ends, per hop
    TimeMs respDelay = 1500;      // T2: wait this long to bundle acks
    TimeMs idleCheck = 180000;    // T3: poll an idle link this often
    int baud = 1200;              // for air-time estimates
    TimeMs txDelay = 300;         // TNC key-up time per transmission
    bool acceptIncoming = false;  // answer SABM with UA (P2P / test RMS)
};

class Link {
public:
    enum class State { Disconnected, Connecting, Connected, Recovery, Disconnecting };

    struct Event {
        enum class Kind { Connected, Data, Disconnected, Log };
        Kind kind;
        Bytes data;        // Data
        std::string text;  // Disconnected: reason ("" for a normal close); Log
    };

    explicit Link(LinkConfig cfg);

    void connect(const Address& remote, std::vector<Address> via, TimeMs now);
    void write(ByteView data, TimeMs now);
    // Graceful: DISC once everything queued has been acknowledged.
    void disconnect(TimeMs now);
    // Immediate: one DISC, then forget the link.
    void abort(TimeMs now);

    void onFrame(const Frame& f, TimeMs now);
    void tick(TimeMs now);

    std::vector<Frame> takeOutgoing() { return std::exchange(out_, {}); }
    std::vector<Event> takeEvents() { return std::exchange(events_, {}); }

    State state() const { return state_; }
    bool connected() const { return state_ == State::Connected || state_ == State::Recovery; }
    const Address& remote() const { return remote_; }
    std::size_t queuedBytes() const;    // written but not yet acknowledged
    std::size_t ackedBytes() const { return acked_; }
    std::optional<TimeMs> nextDeadline() const;

    static const char* stateName(State s);

private:
    struct Sent {
        std::uint8_t ns;
        Bytes data;
    };

    void send(std::uint8_t control, Cr cr, Bytes info, TimeMs now);
    void sendI(const Sent& s, TimeMs now);
    void sendS(std::uint8_t type, Cr cr, bool pf, TimeMs now);
    void sendU(std::uint8_t type, Cr cr, bool pf, TimeMs now);
    void startT1(TimeMs now);
    void pump(TimeMs now);
    bool ackUpTo(std::uint8_t nr, TimeMs now);
    void rewind();
    void established(TimeMs now);
    void down(std::string reason);
    void post(Event::Kind k, std::string text = {}, Bytes data = {});
    TimeMs airtime(std::size_t frameBytes) const;
    bool forMe(const Frame& f) const;
    void replyDm(const Frame& f, bool pf, TimeMs now);
    void maybeFinishDisconnect(TimeMs now);

    LinkConfig cfg_;
    State state_ = State::Disconnected;
    Address remote_;
    std::vector<Address> via_;

    std::uint8_t vs_ = 0, va_ = 0, vr_ = 0;
    std::deque<Sent> unacked_;     // ns = va_, va_+1, ...; the first (vs_-va_) are on the air
    Bytes txq_;                    // written, not yet put in an I-frame
    std::size_t txqHead_ = 0;
    std::size_t acked_ = 0;
    bool remoteBusy_ = false;
    bool rejSent_ = false;
    bool ackPending_ = false;
    bool wantDisconnect_ = false;
    int rc_ = 0;

    std::optional<TimeMs> t1_, t2_, t3_;
    TimeMs txEnd_ = 0;

    std::vector<Frame> out_;
    std::vector<Event> events_;
};

}  // namespace ogm::ax25
