// The radio abstraction: what it takes to get a data path out of a rig.
//
// Phase 1 radios offer a KISS TNC over a byte link (Bluetooth SPP, USB
// serial, or TCP).  A KissTnc object knows how to bring that TNC up and
// put it back afterwards; the channel above it only ever sees KISS frames.
// Phase 2 adds rigs with rig control (frequency, mode, PTT) and an audio
// path for a soundcard modem; they get their own interface next to this one.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ogm/common.hpp"

namespace ogm::radio {

class KissTnc {
public:
    enum class State { Down, Starting, Ready, Failed };

    virtual ~KissTnc() = default;

    // The shell's byte link to the radio just opened.
    virtual void onLinkUp(TimeMs now) = 0;
    // Bytes from the radio outside KISS frames: its replies in command mode.
    virtual void onText(std::string_view line, TimeMs now) = 0;
    // A KISS frame arrived, so the TNC is evidently in KISS mode.
    virtual void onFrameSeen(TimeMs now) = 0;
    virtual void tick(TimeMs now) = 0;
    // Bytes that return the radio to normal when we are done (may be empty).
    virtual Bytes shutdownBytes() const = 0;
    virtual std::string name() const = 0;

    State state() const { return state_; }
    const std::string& status() const { return status_; }
    Bytes takeOut() { return std::exchange(out_, {}); }
    std::vector<std::string> takeLog() { return std::exchange(log_, {}); }

protected:
    void set(State s, std::string status) {
        state_ = s;
        status_ = std::move(status);
        log_.push_back(status_);
    }
    void sendText(std::string_view s) { append(out_, s); }
    void log(std::string s) { log_.push_back(std::move(s)); }

    State state_ = State::Down;
    std::string status_ = "not connected";
    Bytes out_;
    std::vector<std::string> log_;
};

// A TNC that is always in KISS mode: Direwolf, a hardware KISS TNC, a KISS
// TCP port.  Ready as soon as the link is.
class PlainKiss final : public KissTnc {
public:
    void onLinkUp(TimeMs) override { set(State::Ready, "KISS ready"); }
    void onText(std::string_view, TimeMs) override {}
    void onFrameSeen(TimeMs) override {}
    void tick(TimeMs) override {}
    Bytes shutdownBytes() const override { return {}; }
    std::string name() const override { return "KISS TNC"; }
};

// Kenwood TH-D75 (and TH-D74): built-in TNC reached over Bluetooth or USB.
//
// Never assume the radio's state: leave KISS first (a radio in KISS then
// answers like any other), ask for its ID, switch KISS on with "TN 2,<band>"
// and confirm that commands stop being answered.  On shutdown, leave KISS so
// the radio's own APRS and display work again.
class ThD75 final : public KissTnc {
public:
    struct Config {
        int band = 0;                 // 0 = A, 1 = B; the other is tried if refused
        TimeMs probeTimeout = 2000;
    };
    ThD75();
    explicit ThD75(Config cfg);
    void onLinkUp(TimeMs now) override;
    void onText(std::string_view line, TimeMs now) override;
    void onFrameSeen(TimeMs now) override;
    void tick(TimeMs now) override;
    Bytes shutdownBytes() const override;
    std::string name() const override { return "TH-D75"; }

    const std::string& radioId() const { return id_; }

private:
    enum class Step { Idle, Probing, Entering, Confirming };
    Config cfg_;
    Step step_ = Step::Idle;
    int band_ = 0;
    int bandTries_ = 0;
    TimeMs deadline_ = 0;
    std::string id_;
};

// "th-d75" / "th-d74" / "kiss".  nullptr for an unknown model.
std::unique_ptr<KissTnc> makeKissTnc(std::string_view model);

}  // namespace ogm::radio
