// Channels: a reliable byte pipe to one remote station, whatever carries it.
//
// The shell owns the "lower" link (a Bluetooth socket to the radio, a TCP
// socket to the CMS or a modem) and feeds the channel its bytes; the channel
// hands back bytes for that socket and, above, delivers session data.  The
// B2F session only ever sees write() and Data events.
//
//   shell socket <-> [ lower bytes | Channel | session bytes ] <-> B2F
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ogm/ax25.hpp"
#include "ogm/ax25_link.hpp"
#include "ogm/common.hpp"
#include "ogm/kiss.hpp"
#include "ogm/radio.hpp"

namespace ogm {

class Channel {
public:
    struct Event {
        enum class Kind {
            Status,        // text: short, human-readable state ("calling W1AW-10")
            Log,           // text: detail for a log view
            Connected,     // the remote is on the line; the session may start
            Data,          // data: session bytes from the remote
            Disconnected,  // text: why, or "" for a normal end
        };
        Kind kind;
        std::string text;
        Bytes data;
    };

    virtual ~Channel() = default;

    virtual void onLowerUp(TimeMs now) = 0;
    virtual void onLowerDown(TimeMs now) = 0;
    virtual void onLowerBytes(ByteView bytes, TimeMs now) = 0;
    virtual void write(ByteView bytes, TimeMs now) = 0;
    virtual void close(TimeMs now) = 0;  // graceful
    virtual void tick(TimeMs now) = 0;
    virtual Bytes takeLowerOut() = 0;

    // The remote's callsign, for the B2F comment line.
    virtual std::string remoteCall() const = 0;
    virtual std::string describe() const = 0;

    std::vector<Event> takeEvents() { return std::exchange(events_, {}); }
    // Once true, the shell writes any remaining takeLowerOut() and closes its socket.
    bool lowerDone() const { return lowerDone_; }
    bool ended() const { return ended_; }

protected:
    void post(Event::Kind k, std::string text = {}, Bytes data = {}) {
        events_.push_back({k, std::move(text), std::move(data)});
    }
    void end(std::string reason) {
        if (ended_) return;
        ended_ = true;
        lowerDone_ = true;
        post(Event::Kind::Disconnected, std::move(reason));
    }

    std::vector<Event> events_;
    bool lowerDone_ = false;
    bool ended_ = false;
};

// AX.25 connected mode through a KISS TNC (TH-D75 over Bluetooth, Direwolf...).
class Ax25Channel final : public Channel {
public:
    struct Config {
        ax25::LinkConfig link;
        ax25::Address target;
        std::vector<ax25::Address> via;
        bool monitor = false;  // log every frame heard (TNC2 format)
    };
    Ax25Channel(Config cfg, std::unique_ptr<radio::KissTnc> tnc);

    void onLowerUp(TimeMs now) override;
    void onLowerDown(TimeMs now) override;
    void onLowerBytes(ByteView bytes, TimeMs now) override;
    void write(ByteView bytes, TimeMs now) override;
    void close(TimeMs now) override;
    void tick(TimeMs now) override;
    Bytes takeLowerOut() override;
    std::string remoteCall() const override { return cfg_.target.str(false); }
    std::string describe() const override;

    const ax25::Link& link() const { return link_; }

private:
    void drain(TimeMs now);

    Config cfg_;
    std::unique_ptr<radio::KissTnc> tnc_;
    kiss::Decoder decoder_;
    ax25::Link link_;
    bool lowerUp_ = false;
    bool linkStarted_ = false;
    bool closeWanted_ = false;
    Bytes out_;
};

// Telnet to the Winlink CMS (or any host that logs in the same way):
// answer the "Callsign :" and "Password :" prompts, then pass bytes through.
class TelnetChannel final : public Channel {
public:
    struct Config {
        std::string mycall;
        std::string password = "CMSTelnet";  // the CMS's fixed Telnet password, not the account's
        std::string remote = "WL2K";
        TimeMs loginTimeout = 30000;
    };
    explicit TelnetChannel(Config cfg);

    void onLowerUp(TimeMs now) override;
    void onLowerDown(TimeMs now) override;
    void onLowerBytes(ByteView bytes, TimeMs now) override;
    void write(ByteView bytes, TimeMs now) override;
    void close(TimeMs now) override;
    void tick(TimeMs now) override;
    Bytes takeLowerOut() override { return std::exchange(out_, {}); }
    std::string remoteCall() const override { return cfg_.remote; }
    std::string describe() const override { return "Telnet to " + cfg_.remote; }

private:
    Config cfg_;
    bool loggedIn_ = false;
    bool lowerUp_ = false;
    std::optional<TimeMs> deadline_;
    std::string prompt_;
    Bytes out_;
};

}  // namespace ogm
