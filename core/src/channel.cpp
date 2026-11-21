#include "ogm/channel.hpp"

#include <cctype>

namespace ogm {

// ---- Ax25Channel

Ax25Channel::Ax25Channel(Config cfg, std::unique_ptr<radio::KissTnc> tnc)
    : cfg_(std::move(cfg)), tnc_(std::move(tnc)), link_(cfg_.link) {
    if (!tnc_) tnc_ = std::make_unique<radio::PlainKiss>();
}

std::string Ax25Channel::describe() const {
    std::string s = "AX.25 to " + cfg_.target.str(false);
    if (!cfg_.via.empty()) {
        s += " via ";
        for (std::size_t i = 0; i < cfg_.via.size(); ++i) s += (i ? "," : "") + cfg_.via[i].str(false);
    }
    return s + " (" + tnc_->name() + ")";
}

void Ax25Channel::onLowerUp(TimeMs now) {
    lowerUp_ = true;
    post(Event::Kind::Status, "starting the " + tnc_->name());
    tnc_->onLinkUp(now);
    drain(now);
}

void Ax25Channel::onLowerDown(TimeMs now) {
    lowerUp_ = false;
    if (ended()) return;
    link_.abort(now);
    link_.takeOutgoing();
    link_.takeEvents();
    end("lost the connection to the " + tnc_->name());
}

void Ax25Channel::onLowerBytes(ByteView bytes, TimeMs now) {
    for (auto& item : decoder_.feed(bytes)) {
        if (item.kind == kiss::Decoder::Item::Kind::Text) {
            tnc_->onText(asText(item.data), now);
            continue;
        }
        if ((item.command & 0x0F) != 0) continue;  // not a data frame
        tnc_->onFrameSeen(now);
        std::string err;
        auto f = ax25::decode(item.data, &err);
        if (!f) {
            post(Event::Kind::Log, "undecodable frame: " + err);
            continue;
        }
        if (cfg_.monitor) post(Event::Kind::Log, "heard " + f->tnc2());
        link_.onFrame(*f, now);
    }
    drain(now);
}

void Ax25Channel::write(ByteView bytes, TimeMs now) {
    link_.write(bytes, now);
    drain(now);
}

void Ax25Channel::close(TimeMs now) {
    closeWanted_ = true;
    if (!linkStarted_ || link_.state() == ax25::Link::State::Disconnected) {
        append(out_, tnc_->shutdownBytes());
        end("");
        return;
    }
    link_.disconnect(now);
    drain(now);
}

void Ax25Channel::tick(TimeMs now) {
    if (ended() || !lowerUp_) return;
    tnc_->tick(now);
    link_.tick(now);
    drain(now);
}

void Ax25Channel::drain(TimeMs now) {
    for (auto& line : tnc_->takeLog()) post(Event::Kind::Log, std::move(line));
    append(out_, tnc_->takeOut());
    if (ended()) return;

    if (tnc_->state() == radio::KissTnc::State::Failed) {
        end(tnc_->status());
        return;
    }
    if (tnc_->state() == radio::KissTnc::State::Ready && !linkStarted_ && !closeWanted_) {
        linkStarted_ = true;
        post(Event::Kind::Status, "calling " + cfg_.target.str(false));
        link_.connect(cfg_.target, cfg_.via, now);
    }

    for (auto& f : link_.takeOutgoing()) {
        if (cfg_.monitor) post(Event::Kind::Log, "sent  " + f.tnc2());
        if (auto bytes = ax25::encode(f)) append(out_, kiss::wrap(*bytes));
    }
    for (auto& e : link_.takeEvents()) {
        switch (e.kind) {
            case ax25::Link::Event::Kind::Connected:
                post(Event::Kind::Status, "connected to " + link_.remote().str(false));
                post(Event::Kind::Connected);
                break;
            case ax25::Link::Event::Kind::Data: post(Event::Kind::Data, {}, std::move(e.data)); break;
            case ax25::Link::Event::Kind::Log: post(Event::Kind::Log, std::move(e.text)); break;
            case ax25::Link::Event::Kind::Disconnected:
                // The frames the link just queued (a final DISC/UA) go out first.
                for (auto& f : link_.takeOutgoing())
                    if (auto bytes = ax25::encode(f)) append(out_, kiss::wrap(*bytes));
                append(out_, tnc_->shutdownBytes());
                end(std::move(e.text));
                return;
        }
    }
}

Bytes Ax25Channel::takeLowerOut() { return std::exchange(out_, {}); }

// ---- TelnetChannel

TelnetChannel::TelnetChannel(Config cfg) : cfg_(std::move(cfg)) {}

void TelnetChannel::onLowerUp(TimeMs now) {
    lowerUp_ = true;
    deadline_ = now + cfg_.loginTimeout;
    post(Event::Kind::Status, "logging in");
}

void TelnetChannel::onLowerDown(TimeMs) {
    lowerUp_ = false;
    end(loggedIn_ ? "" : "the server closed the connection before login");
}

void TelnetChannel::onLowerBytes(ByteView bytes, TimeMs now) {
    std::size_t i = 0;
    while (!loggedIn_ && i < bytes.size()) {
        char c = static_cast<char>(bytes[i++]);
        if (c == '\r' || c == '\n') {
            if (!prompt_.empty()) post(Event::Kind::Log, "<" + prompt_);
            prompt_.clear();
            continue;
        }
        prompt_ += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        // Prompts end in ':' and may arrive without a line ending.
        std::string_view p = prompt_;
        while (!p.empty() && p.back() == ' ') p.remove_suffix(1);
        if (p.empty() || p.back() != ':') continue;
        while (!p.empty() && p.front() == ' ') p.remove_prefix(1);
        if (p.substr(0, 8) == "callsign") {
            append(out_, cfg_.mycall + "\r");
            post(Event::Kind::Log, ">" + cfg_.mycall);
            prompt_.clear();
        } else if (p.substr(0, 8) == "password") {
            append(out_, cfg_.password + "\r");
            post(Event::Kind::Log, ">(password)");
            prompt_.clear();
            loggedIn_ = true;
            deadline_.reset();
            post(Event::Kind::Status, "connected to " + cfg_.remote);
            post(Event::Kind::Connected);
        }
    }
    if (loggedIn_ && i < bytes.size()) post(Event::Kind::Data, {}, Bytes(bytes.begin() + static_cast<std::ptrdiff_t>(i), bytes.end()));
    (void)now;
}

void TelnetChannel::write(ByteView bytes, TimeMs) {
    if (loggedIn_) append(out_, bytes);
}

void TelnetChannel::close(TimeMs) { end(""); }

void TelnetChannel::tick(TimeMs now) {
    if (deadline_ && now >= *deadline_ && !ended()) end("no login prompt from the server");
}

}  // namespace ogm
