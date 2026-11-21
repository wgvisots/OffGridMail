// The front door for the shells: compose mail, run one session at a time
// over a channel, and report what happened.
//
// A session goes:  startSession(channel)  ->  the shell opens its socket and
// calls onLowerUp  ->  the channel reaches the gateway  ->  B2F exchanges
// mail  ->  the channel closes  ->  SessionEnded.  Between calls the shell
// keeps calling tick(now) (every 100 ms or so), writes takeLowerOut() to its
// socket, and closes the socket once lowerDone() is true.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ogm/b2f.hpp"
#include "ogm/channel.hpp"
#include "ogm/common.hpp"
#include "ogm/mailbox.hpp"
#include "ogm/message.hpp"

namespace ogm {

struct EngineConfig {
    std::string mycall;    // e.g. "N0CALL" (no SSID: the Winlink account)
    std::string password;  // Winlink account password, for secure login
    std::string locator;   // grid square, optional
    std::string appVersion = "0.1";
};

class Engine {
public:
    struct Event {
        enum class Kind {
            Status,        // text: what the session is doing now
            Log,           // text: protocol detail
            Received,      // mid: a new message is in the inbox
            AccountActivated,  // mid, text: the new account's password (may be "" if not found)
            Sent,          // mid: delivered (moved to sent)
            Progress,      // done/total bytes of the message being received
            SessionEnded,  // ok, text: summary or reason
        };
        Kind kind;
        std::string mid;
        std::string text;
        bool ok = true;
        std::size_t done = 0, total = 0;
    };

    Engine(EngineConfig cfg, Mailbox& mailbox);

    // Mail.  Returns the new message's MID (it waits in the outbox), or ""
    // with *error set if the message is not valid.
    std::string compose(const Message::Draft& draft, std::int64_t unixSeconds, std::string* error = nullptr);

    // Sessions.
    bool startSession(std::unique_ptr<Channel> channel, TimeMs now);
    bool sessionActive() const { return channel_ != nullptr; }
    void abortSession(TimeMs now);  // stop politely, as soon as possible

    // The shell's socket.
    void onLowerUp(TimeMs now);
    void onLowerDown(TimeMs now);
    void onLowerBytes(ByteView bytes, TimeMs now);
    void tick(TimeMs now);
    Bytes takeLowerOut();
    bool lowerDone() const { return lowerDone_; }

    std::vector<Event> takeEvents() { return std::exchange(events_, {}); }
    Mailbox& mailbox() { return mailbox_; }
    const EngineConfig& config() const { return cfg_; }

private:
    void pumpChannel(TimeMs now);
    void pumpSession(TimeMs now);
    void endSession(bool ok, std::string text);
    void post(Event::Kind k, std::string mid = {}, std::string text = {});

    EngineConfig cfg_;
    Mailbox& mailbox_;
    std::uint64_t composeCount_ = 0;

    std::unique_ptr<Channel> channel_;
    std::unique_ptr<b2f::Session> session_;
    Bytes lowerOut_;
    bool lowerDone_ = false;
    bool closing_ = false;
    int received_ = 0, sent_ = 0;
    std::string failure_;
    std::vector<Event> events_;
};

}  // namespace ogm
