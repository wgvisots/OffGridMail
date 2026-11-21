// A pretend Winlink RMS gateway, built from the same core: AX.25 (accepting
// calls) over KISS, or the CMS Telnet login, then a B2F master with secure
// login.  Messages in its mailbox's out/ folder are offered to whoever
// connects; what it receives lands in in/.
//
// No I/O, like the core: the test or the ogmfake tool moves the bytes.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ogm/ax25_link.hpp"
#include "ogm/b2f.hpp"
#include "ogm/kiss.hpp"
#include "ogm/mailbox.hpp"

namespace ogm::sim {

class FakeRms {
public:
    struct Config {
        std::string call = "W1AW-10";
        std::string password;           // empty: no secure login
        std::string challenge = "12345678";
        bool telnet = false;            // Telnet login instead of AX.25 over KISS
        int dropEvery = 0;              // AX.25: lose every Nth frame we would send (0 = none)
    };

    FakeRms(Config cfg, Mailbox& mailbox);

    void onClientConnected(TimeMs now);   // a TCP client arrived
    void onBytes(ByteView bytes, TimeMs now);
    void tick(TimeMs now);
    Bytes takeOut() { return std::exchange(out_, {}); }
    std::vector<std::string> takeLog() { return std::exchange(log_, {}); }

    // Telnet: the session is over and the server would hang up.
    bool wantsHangup() const { return hangup_; }
    int sessions() const { return sessions_; }
    std::vector<std::string> received() const { return received_; }

private:
    void startSession(TimeMs now);
    void pumpSession(TimeMs now);
    void pumpLink(TimeMs now);

    Config cfg_;
    Mailbox& mailbox_;
    kiss::Decoder decoder_;
    ax25::Link link_;
    std::unique_ptr<b2f::Session> session_;
    Bytes out_;
    std::vector<std::string> log_;
    std::vector<std::string> received_;
    int sessions_ = 0;
    int frameCount_ = 0;

    // Telnet login
    int loginStep_ = 0;  // 0 none, 1 asked callsign, 2 asked password, 3 in session
    std::string lineBuf_;
    bool hangup_ = false;
    bool closing_ = false;
};

}  // namespace ogm::sim
