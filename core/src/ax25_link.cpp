#include "ogm/ax25_link.hpp"

#include <algorithm>

namespace ogm::ax25 {
namespace {

// Control field, modulo 8.
constexpr std::uint8_t RR = 0x01, RNR = 0x05, REJ = 0x09, SREJ = 0x0D;
constexpr std::uint8_t SABM = 0x2F, SABME = 0x6F, DISC = 0x43, DM = 0x0F, UA = 0x63, FRMR = 0x87, UI = 0x03;
constexpr std::uint8_t PF = 0x10;

std::uint8_t mod8(int v) { return static_cast<std::uint8_t>(v & 7); }
bool isI(std::uint8_t c) { return (c & 0x01) == 0; }
bool isS(std::uint8_t c) { return (c & 0x03) == 0x01; }
std::uint8_t nsOf(std::uint8_t c) { return (c >> 1) & 7; }
std::uint8_t nrOf(std::uint8_t c) { return (c >> 5) & 7; }
bool pfOf(std::uint8_t c) { return (c & PF) != 0; }

}  // namespace

const char* Link::stateName(State s) {
    switch (s) {
        case State::Disconnected: return "disconnected";
        case State::Connecting: return "connecting";
        case State::Connected: return "connected";
        case State::Recovery: return "waiting for an answer";
        case State::Disconnecting: return "disconnecting";
    }
    return "?";
}

Link::Link(LinkConfig cfg) : cfg_(std::move(cfg)) {
    cfg_.maxframe = std::clamp<std::size_t>(cfg_.maxframe, 1, 7);
    cfg_.paclen = std::clamp<std::size_t>(cfg_.paclen, 16, 256);
    if (cfg_.retries < 1) cfg_.retries = 1;
}

void Link::post(Event::Kind k, std::string text, Bytes data) {
    events_.push_back({k, std::move(data), std::move(text)});
}

std::size_t Link::queuedBytes() const {
    std::size_t n = txq_.size() - txqHead_;
    for (const auto& s : unacked_) n += s.data.size();
    return n;
}

TimeMs Link::airtime(std::size_t frameBytes) const {
    // Flags and FCS add 4 bytes; bit stuffing adds up to 1/5 (call it 5%).
    std::size_t bits = (frameBytes + 4) * 8 * 105 / 100;
    return static_cast<TimeMs>(bits * 1000 / static_cast<std::size_t>(std::max(cfg_.baud, 1)));
}

void Link::send(std::uint8_t control, Cr cr, Bytes info, TimeMs now) {
    Frame f;
    f.dest = remote_;
    f.src = cfg_.mycall;
    f.path = via_;
    for (auto& a : f.path) a.hBit = false;
    f.cr = cr;
    f.control = control;
    if (hasPid(control)) {
        f.pid = kPidNoLayer3;
    } else {
        f.pid.reset();
    }
    f.info = std::move(info);
    std::size_t bytes = 7 * (2 + f.path.size()) + 2 + f.info.size();
    // Back-to-back frames share one key-up; a new transmission pays TXDELAY.
    TimeMs start = now >= txEnd_ ? now + cfg_.txDelay : txEnd_;
    txEnd_ = start + airtime(bytes);
    out_.push_back(std::move(f));
}

void Link::sendI(const Sent& s, TimeMs now) {
    std::uint8_t c = static_cast<std::uint8_t>((vr_ << 5) | (s.ns << 1));
    send(c, Cr::Command, s.data, now);
    ackPending_ = false;  // N(R) rides along
    t2_.reset();
}

void Link::sendS(std::uint8_t type, Cr cr, bool pf, TimeMs now) {
    send(static_cast<std::uint8_t>((vr_ << 5) | (pf ? PF : 0) | type), cr, {}, now);
    ackPending_ = false;
    t2_.reset();
}

void Link::sendU(std::uint8_t type, Cr cr, bool pf, TimeMs now) {
    send(static_cast<std::uint8_t>(type | (pf ? PF : 0)), cr, {}, now);
}

void Link::startT1(TimeMs now) {
    // Every digipeater repeats our frame and the answer: one more frack per hop each way.
    TimeMs wait = cfg_.frack * static_cast<TimeMs>(1 + 2 * via_.size());
    t1_ = std::max(now, txEnd_) + wait;
}

void Link::connect(const Address& remote, std::vector<Address> via, TimeMs now) {
    remote_ = remote;
    via_ = std::move(via);
    vs_ = va_ = vr_ = 0;
    unacked_.clear();
    remoteBusy_ = rejSent_ = ackPending_ = wantDisconnect_ = false;
    rc_ = 0;
    t2_.reset();
    t3_.reset();
    state_ = State::Connecting;
    post(Event::Kind::Log, "connecting to " + remote_.str() +
                               (via_.empty() ? "" : " via " + [&] {
                                   std::string s;
                                   for (const auto& a : via_) s += (s.empty() ? "" : ",") + a.str(false);
                                   return s;
                               }()));
    sendU(SABM, Cr::Command, true, now);
    startT1(now);
}

void Link::write(ByteView data, TimeMs now) {
    if (txqHead_ > 4096 && txqHead_ * 2 > txq_.size()) {
        txq_.erase(txq_.begin(), txq_.begin() + static_cast<std::ptrdiff_t>(txqHead_));
        txqHead_ = 0;
    }
    append(txq_, data);
    pump(now);
}

void Link::disconnect(TimeMs now) {
    switch (state_) {
        case State::Disconnected:
        case State::Disconnecting: return;
        case State::Connecting:
            abort(now);
            return;
        default:
            wantDisconnect_ = true;
            maybeFinishDisconnect(now);
    }
}

void Link::maybeFinishDisconnect(TimeMs now) {
    if (!wantDisconnect_ || state_ != State::Connected) return;
    if (txqHead_ < txq_.size() || !unacked_.empty()) return;
    if (ackPending_) sendS(RR, Cr::Response, false, now);
    state_ = State::Disconnecting;
    rc_ = 0;
    t3_.reset();
    sendU(DISC, Cr::Command, true, now);
    startT1(now);
}

void Link::abort(TimeMs now) {
    if (state_ == State::Disconnected) return;
    sendU(DISC, Cr::Command, true, now);
    down("");
}

void Link::down(std::string reason) {
    state_ = State::Disconnected;
    t1_.reset();
    t2_.reset();
    t3_.reset();
    unacked_.clear();
    txq_.clear();
    txqHead_ = 0;
    wantDisconnect_ = false;
    post(Event::Kind::Disconnected, std::move(reason));
}

void Link::established(TimeMs now) {
    state_ = State::Connected;
    vs_ = va_ = vr_ = 0;
    rc_ = 0;
    remoteBusy_ = rejSent_ = false;
    t1_.reset();
    t3_ = now + cfg_.idleCheck;
    post(Event::Kind::Connected);
    pump(now);
}

void Link::rewind() { vs_ = va_; }

bool Link::ackUpTo(std::uint8_t nr, TimeMs now) {
    // Valid if va <= nr <= vs (mod 8).
    if (mod8(nr - va_) > mod8(vs_ - va_)) {
        post(Event::Kind::Log, "ignoring out-of-window acknowledgement N(R)=" + std::to_string(nr));
        return false;
    }
    bool progress = false;
    while (va_ != nr) {
        acked_ += unacked_.front().data.size();
        unacked_.pop_front();
        va_ = mod8(va_ + 1);
        progress = true;
    }
    if (va_ == vs_) {
        if (state_ == State::Connected) {
            t1_.reset();
            t3_ = now + cfg_.idleCheck;
        }
    } else if (progress && state_ == State::Connected) {
        startT1(now);
    }
    return true;
}

void Link::pump(TimeMs now) {
    if (state_ != State::Connected || remoteBusy_) return;
    bool sentAny = false;
    // Frames already numbered but not (re)sent since a rewind go first.
    while (mod8(vs_ - va_) < unacked_.size()) {
        sendI(unacked_[mod8(vs_ - va_)], now);
        vs_ = mod8(vs_ + 1);
        sentAny = true;
    }
    while (unacked_.size() < cfg_.maxframe && txqHead_ < txq_.size()) {
        std::size_t n = std::min(cfg_.paclen, txq_.size() - txqHead_);
        Sent s{vs_, Bytes(txq_.begin() + static_cast<std::ptrdiff_t>(txqHead_),
                          txq_.begin() + static_cast<std::ptrdiff_t>(txqHead_ + n))};
        txqHead_ += n;
        unacked_.push_back(std::move(s));
        sendI(unacked_.back(), now);
        vs_ = mod8(vs_ + 1);
        sentAny = true;
    }
    if (txqHead_ == txq_.size()) {
        txq_.clear();
        txqHead_ = 0;
    }
    if (sentAny) {
        t3_.reset();
        startT1(now);  // from the end of this burst
    }
    maybeFinishDisconnect(now);
}

// DM to a station we have no link with, without disturbing the current link.
void Link::replyDm(const Frame& f, bool pf, TimeMs now) {
    Address savedRemote = std::exchange(remote_, f.src);
    std::vector<Address> savedVia = std::exchange(via_, std::vector<Address>(f.path.rbegin(), f.path.rend()));
    sendU(DM, Cr::Response, pf, now);
    remote_ = std::move(savedRemote);
    via_ = std::move(savedVia);
}

bool Link::forMe(const Frame& f) const {
    return f.dest.sameStation(cfg_.mycall) && f.fullyRepeated();
}

void Link::onFrame(const Frame& f, TimeMs now) {
    if (!forMe(f)) return;
    std::uint8_t c = f.control;
    bool pf = pfOf(c);
    // v1 stations leave the C bits alone; guess from the frame type.
    bool command = f.cr == Cr::Command ||
                   ((f.cr == Cr::Legacy || f.cr == Cr::LegacySet) &&
                    (isI(c) || (c & ~PF) == SABM || (c & ~PF) == DISC || (isS(c) && pf && state_ != State::Recovery)));
    bool fromPeer = f.src.sameStation(remote_) && state_ != State::Disconnected;

    // Unnumbered frames.
    if (!isI(c) && !isS(c)) {
        std::uint8_t u = c & static_cast<std::uint8_t>(~PF);
        if (u == UI) return;  // APRS and beacons: not ours to handle
        if (u == SABM || u == SABME) {
            if (fromPeer && state_ == State::Connecting) {  // both called at once
                sendU(UA, Cr::Response, pf, now);
                established(now);
            } else if (fromPeer && state_ != State::Disconnecting) {
                post(Event::Kind::Log, "link reset by " + remote_.str());
                sendU(UA, Cr::Response, pf, now);
                unacked_.clear();
                established(now);
            } else if (state_ == State::Disconnected && cfg_.acceptIncoming && u == SABM) {
                remote_ = f.src;
                via_.assign(f.path.rbegin(), f.path.rend());
                for (auto& a : via_) a.hBit = false;
                txq_.clear();
                txqHead_ = 0;
                unacked_.clear();
                wantDisconnect_ = false;
                sendU(UA, Cr::Response, pf, now);
                established(now);
            } else {
                // Busy, or not taking calls (SABME: we only do modulo 8).
                replyDm(f, pf, now);
            }
            return;
        }
        if (!fromPeer) {
            if (u == DISC && state_ == State::Disconnected) {
                replyDm(f, pf, now);
            }
            return;
        }
        switch (u) {
            case UA:
                if (state_ == State::Connecting) {
                    established(now);
                } else if (state_ == State::Disconnecting) {
                    down("");
                }
                return;
            case DM:
                if (state_ == State::Connecting) {
                    down(remote_.str() + " is busy or refused the connection");
                } else if (state_ == State::Disconnecting) {
                    down("");
                } else {
                    down(remote_.str() + " dropped the link");
                }
                return;
            case DISC:
                sendU(UA, Cr::Response, pf, now);
                if (state_ != State::Disconnecting) post(Event::Kind::Log, remote_.str() + " closed the link");
                down("");
                return;
            case FRMR:
                post(Event::Kind::Log, remote_.str() + " rejected a frame (FRMR)");
                sendU(DISC, Cr::Command, true, now);
                down(remote_.str() + " reported a protocol error");
                return;
            default: return;
        }
    }

    if (!fromPeer) {
        // A numbered frame for a link we don't have: tell the sender (only if polled).
        if (state_ == State::Disconnected && command && pf) {
            replyDm(f, true, now);
        }
        return;
    }
    // While connecting or disconnecting only U frames matter.
    if (state_ != State::Connected && state_ != State::Recovery) return;

    if (isI(c)) {
        ackUpTo(nrOf(c), now);
        if (nsOf(c) == vr_) {
            vr_ = mod8(vr_ + 1);
            rejSent_ = false;
            if (!f.info.empty()) post(Event::Kind::Data, {}, f.info);
            if (pf) {
                sendS(RR, Cr::Response, true, now);
            } else if (!ackPending_) {
                ackPending_ = true;
                t2_ = now + cfg_.respDelay;
            }
        } else if (!rejSent_) {
            rejSent_ = true;
            sendS(REJ, Cr::Response, pf, now);
        } else if (pf) {
            sendS(RR, Cr::Response, true, now);
        }
        pump(now);
        return;
    }

    // Supervisory: RR, RNR, REJ (SREJ treated as REJ: we resend from N(R)).
    std::uint8_t type = c & 0x0F;
    std::uint8_t nr = nrOf(c);
    remoteBusy_ = type == RNR;
    if (command && pf) sendS(RR, Cr::Response, true, now);  // answer the enquiry

    if (state_ == State::Recovery) {
        if (!command && pf) {
            // The answer to our poll: it tells us exactly what arrived.
            if (!ackUpTo(nr, now)) return;
            t1_.reset();
            rc_ = 0;
            state_ = State::Connected;
            rewind();
            if (unacked_.empty()) {
                t3_ = now + cfg_.idleCheck;
            } else if (remoteBusy_) {
                startT1(now);
            }
            pump(now);
        } else {
            ackUpTo(nr, now);
        }
        return;
    }

    if (!ackUpTo(nr, now)) return;
    if (type == REJ || type == SREJ) {
        rewind();
        t1_.reset();
    }
    if (remoteBusy_ && !unacked_.empty() && !t1_) startT1(now);
    pump(now);
}

void Link::tick(TimeMs now) {
    if (t2_ && now >= *t2_) {
        t2_.reset();
        if (ackPending_ && connected()) sendS(RR, Cr::Response, false, now);
    }
    if (t1_ && now >= *t1_) {
        t1_.reset();
        switch (state_) {
            case State::Connecting:
                if (++rc_ >= cfg_.retries) {
                    down("no answer from " + remote_.str() + " after " + std::to_string(rc_) + " tries");
                } else {
                    post(Event::Kind::Log, "no answer yet, calling again (" + std::to_string(rc_ + 1) + ")");
                    sendU(SABM, Cr::Command, true, now);
                    startT1(now);
                }
                break;
            case State::Connected:
                state_ = State::Recovery;
                rc_ = 1;
                sendS(RR, Cr::Command, true, now);
                startT1(now);
                break;
            case State::Recovery:
                if (rc_ >= cfg_.retries) {
                    sendU(DISC, Cr::Command, true, now);
                    down("lost the link to " + remote_.str() + " (no answer after " + std::to_string(rc_) + " polls)");
                } else {
                    ++rc_;
                    sendS(RR, Cr::Command, true, now);
                    startT1(now);
                }
                break;
            case State::Disconnecting:
                if (++rc_ >= cfg_.retries / 2 + 1) {
                    down("");  // we tried to be polite
                } else {
                    sendU(DISC, Cr::Command, true, now);
                    startT1(now);
                }
                break;
            case State::Disconnected: break;
        }
    }
    if (t3_ && now >= *t3_ && state_ == State::Connected) {
        t3_.reset();
        state_ = State::Recovery;
        rc_ = 1;
        sendS(RR, Cr::Command, true, now);
        startT1(now);
    }
}

std::optional<TimeMs> Link::nextDeadline() const {
    std::optional<TimeMs> d;
    for (const auto& t : {t1_, t2_, t3_})
        if (t && (!d || *t < *d)) d = t;
    return d;
}

}  // namespace ogm::ax25
