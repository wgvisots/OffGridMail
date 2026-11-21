// The B2 forwarding protocol (B2F) that Winlink uses over every channel.
//
// A Session is driven entirely by bytes: feed it what the channel delivered
// (onData), send what it hands back (takeOut), and read its events.  It works
// the same over AX.25, Telnet, or a modem, and plays either role:
//
//   client  (we called a gateway): waits for the gateway's SID and prompt,
//           answers with ;FW, our SID and the secure-login response, then
//           takes the first turn.
//   master  (we were called, or a test RMS): sends SID, optional ;PQ
//           challenge and prompt, then waits for the client's first turn.
//
// Each turn is a block of up to 5 proposals ("FC EM <mid> <size> <csize> 0",
// then "F> <checksum>"), or FF (nothing to send), or FQ (quit).  Accepted
// messages follow as LZHUF-compressed binary blocks.
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ogm/common.hpp"

namespace ogm::b2f {

struct Config {
    std::string mycall;          // our callsign, upper case
    std::string targetcall;      // the gateway's call (for the comment line)
    std::string locator;         // Maidenhead grid, may be empty
    std::string password;        // Winlink password, for ;PQ challenges
    bool master = false;
    std::string appName = "OffGridMail";  // no dashes: the SID is NAME-VERSION-FEATURES
    std::string appVersion = "0.1";
    std::size_t blockSize = 125;  // data bytes per STX block; 125 fits a 128-byte AX.25 frame
    // Master only: challenge to send as ";PQ:" (empty = no secure login).
    std::string challenge;
};

// One outbound message.
struct Outgoing {
    std::string mid;
    std::string title;  // ASCII (the raw Subject header is fine: it is already RFC 2047 encoded)
    Bytes raw;          // serialized Winlink message
};

// What the remote told us about a message waiting for us (";PM:" lines).
struct Pending {
    std::string to, mid, from, subject;
    std::size_t size = 0;
};

struct Event {
    enum class Kind {
        Log,          // text: one protocol line, '<' received or '>' sent
        Handshake,    // text: remote SID
        Received,     // mid, data: decompressed message, ready to store
        Sent,         // mid: the remote confirmed it
        Rejected,     // mid: remote already has it (counts as delivered)
        Deferred,     // mid: remote wants it later
        Progress,     // mid, done/total compressed bytes (receive side)
        Done,         // session ended normally
        Failed,       // text: why
    };
    Kind kind;
    std::string mid;
    std::string text;
    Bytes data;
    std::size_t done = 0, total = 0;
};

class Session {
public:
    // haveMid answers "is this message already in the mailbox?", so a
    // re-proposed message is refused ('-') instead of downloaded twice.
    Session(Config cfg, std::vector<Outgoing> outbound, std::function<bool(const std::string&)> haveMid);

    // Called with each received message *before* the session takes its next
    // turn, because taking the turn is what tells the remote "delivered".
    // Return false (and set why) if the message could not be stored: the
    // session then reports an error to the remote and stops, so the remote
    // keeps the message for next time.
    using Store = std::function<bool(const std::string& mid, const Bytes& data, std::string& why)>;
    void setStore(Store store) { store_ = std::move(store); }

    // The master speaks first; the client just waits for data.
    void start();
    void onData(ByteView bytes);

    Bytes takeOut();
    std::vector<Event> takeEvents();

    bool finished() const { return state_ == State::Done || state_ == State::Failed; }
    bool failed() const { return state_ == State::Failed; }
    // True once we sent or received FQ: the caller should disconnect.
    bool quitting() const { return quit_; }
    const std::map<std::string, Pending>& pending() const { return pending_; }
    const std::string& remoteSid() const { return remoteSid_; }

private:
    enum class State {
        Handshake,     // reading the remote's SID/prompt (client) or reply (master)
        AwaitAnswer,   // sent proposals, waiting for FS
        RemoteTurn,    // reading proposals / FF / FQ
        Receiving,     // binary blocks of accepted messages
        Done,
        Failed,
    };
    enum class Bin { Soh, HeaderLen, Title, Offset, Type, Len, Data, Checksum };

    struct Proposal {
        char code = 'C';
        std::string type = "EM";
        std::string mid;
        std::size_t size = 0, csize = 0;
        char answer = '=';
        Bytes compressed;
        std::string title;
    };

    void line(std::string_view text);
    void handshakeLine(std::string_view text);
    void sendHandshake();
    void ourTurn();
    void remoteTurnLine(std::string_view text);
    void answerProposals();
    void sendMessage(const Proposal& p);
    void binaryByte(std::uint8_t b);
    void finishReceived();
    void confirmSent();
    void fail(std::string why);
    void finish();
    void sendLine(std::string_view text);
    void post(Event::Kind k, std::string mid = {}, std::string text = {});

    Config cfg_;
    std::vector<Outgoing> outgoing_;
    std::function<bool(const std::string&)> haveMid_;
    Store store_;
    State state_ = State::Handshake;
    std::string lineBuf_;
    Bytes out_;
    std::vector<Event> events_;

    std::string remoteSid_;
    std::string challengeSeen_;       // client: the ;PQ we answer
    bool gotResponse_ = false;        // master: saw a valid ;PR
    bool remoteNoMsgs_ = false;       // remote's last turn was FF
    bool quit_ = false;

    std::vector<Proposal> sentBlock_;     // our proposals awaiting FS / confirmation
    std::vector<std::string> awaitingConfirm_;
    std::vector<Proposal> inBlock_;       // remote proposals of the current turn
    unsigned inChecksum_ = 0;
    std::size_t recvIndex_ = 0;           // into inBlock_, accepted only
    std::map<std::string, Pending> pending_;

    Bin bin_ = Bin::Soh;
    std::size_t binHeaderLen_ = 0, binRemain_ = 0;
    std::string binTitle_, binOffset_;
    unsigned binSum_ = 0;
    Bytes binData_;
};

// Exposed for tests.
std::string formatSid(const std::string& name, const std::string& version);
unsigned proposalChecksum(const std::vector<std::string>& lines);

}  // namespace ogm::b2f
