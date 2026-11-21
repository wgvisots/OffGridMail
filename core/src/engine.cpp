#include "ogm/engine.hpp"

#include <cctype>

namespace ogm {

Engine::Engine(EngineConfig cfg, Mailbox& mailbox) : cfg_(std::move(cfg)), mailbox_(mailbox) {
    for (char& c : cfg_.mycall) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

void Engine::post(Event::Kind k, std::string mid, std::string text) {
    Event e;
    e.kind = k;
    e.mid = std::move(mid);
    e.text = std::move(text);
    events_.push_back(std::move(e));
}

std::string Engine::compose(const Message::Draft& draft, std::int64_t unixSeconds, std::string* error) {
    Message::Draft d = draft;
    if (d.from.empty()) d.from = cfg_.mycall;
    Message m = Message::compose(d, unixSeconds, ++composeCount_);
    while (mailbox_.has(m.mid())) m = Message::compose(d, unixSeconds, ++composeCount_);
    if (auto why = m.validate(); !why.empty()) {
        if (error) *error = why;
        return {};
    }
    if (!mailbox_.save(Mailbox::Folder::Out, m.mid(), m.serialize())) {
        if (error) *error = "could not write to the outbox";
        return {};
    }
    return m.mid();
}

bool Engine::startSession(std::unique_ptr<Channel> channel, TimeMs) {
    if (channel_ || !channel) return false;
    channel_ = std::move(channel);
    session_.reset();
    lowerOut_.clear();
    lowerDone_ = false;
    closing_ = false;
    received_ = sent_ = 0;
    failure_.clear();
    post(Event::Kind::Status, {}, "starting: " + channel_->describe());
    return true;
}

void Engine::abortSession(TimeMs now) {
    if (!channel_) return;
    if (failure_.empty()) failure_ = "stopped by the user";
    closing_ = true;
    channel_->close(now);
    pumpChannel(now);
}

void Engine::onLowerUp(TimeMs now) {
    if (!channel_) return;
    channel_->onLowerUp(now);
    pumpChannel(now);
}

void Engine::onLowerDown(TimeMs now) {
    if (!channel_) return;
    channel_->onLowerDown(now);
    pumpChannel(now);
}

void Engine::onLowerBytes(ByteView bytes, TimeMs now) {
    if (!channel_) return;
    channel_->onLowerBytes(bytes, now);
    pumpChannel(now);
}

void Engine::tick(TimeMs now) {
    if (!channel_) return;
    channel_->tick(now);
    pumpChannel(now);
}

Bytes Engine::takeLowerOut() { return std::exchange(lowerOut_, {}); }

void Engine::pumpChannel(TimeMs now) {
    // Channel events can start or feed the session, whose output goes back
    // into the channel, so go round until both are quiet.
    for (int guard = 0; channel_ && guard < 16; ++guard) {
        auto events = channel_->takeEvents();
        if (events.empty()) break;
        for (auto& e : events) {
            switch (e.kind) {
                case Channel::Event::Kind::Status: post(Event::Kind::Status, {}, std::move(e.text)); break;
                case Channel::Event::Kind::Log: post(Event::Kind::Log, {}, std::move(e.text)); break;
                case Channel::Event::Kind::Connected: {
                    std::vector<b2f::Outgoing> out;
                    for (const auto& mid : mailbox_.list(Mailbox::Folder::Out)) {
                        auto raw = mailbox_.load(Mailbox::Folder::Out, mid);
                        if (!raw) continue;
                        auto m = Message::parse(*raw);
                        if (!m) {
                            post(Event::Kind::Log, mid, "skipping unreadable outbox message " + mid);
                            continue;
                        }
                        out.push_back({m->mid(), m->header("Subject"), std::move(*raw)});
                    }
                    b2f::Config bc;
                    bc.mycall = cfg_.mycall;
                    bc.targetcall = channel_->remoteCall();
                    bc.locator = cfg_.locator;
                    bc.password = cfg_.password;
                    bc.appVersion = cfg_.appVersion;
                    std::size_t n = out.size();
                    session_ = std::make_unique<b2f::Session>(bc, std::move(out),
                                                              [this](const std::string& mid) { return mailbox_.has(mid); });
                    session_->setStore([this](const std::string& mid, const Bytes& data, std::string& why) {
                        if (mailbox_.save(Mailbox::Folder::In, mid, data)) return true;
                        auto copy = mailbox_.rescue(mid, data);
                        why = "cannot store message " + mid;
                        post(Event::Kind::Log, mid,
                             "could not save " + mid + (copy.empty() ? std::string(" anywhere")
                                                                     : "; a copy is in " + copy.string()));
                        return false;
                    });
                    session_->start();
                    post(Event::Kind::Status, {},
                         "exchanging mail (" + std::to_string(n) + " to send)");
                    break;
                }
                case Channel::Event::Kind::Data:
                    if (session_ && !session_->finished()) session_->onData(e.data);
                    break;
                case Channel::Event::Kind::Disconnected: {
                    bool sessionOk = session_ && session_->finished() && !session_->failed();
                    std::string why = e.text;
                    if (!failure_.empty()) {
                        endSession(false, failure_);
                    } else if (sessionOk) {
                        endSession(true, "");
                    } else if (!why.empty()) {
                        endSession(false, why);
                    } else if (session_) {
                        endSession(false, "the gateway hung up before the exchange finished");
                    } else {
                        endSession(false, "the connection closed before the gateway answered");
                    }
                    break;
                }
            }
        }
        pumpSession(now);
    }
    if (channel_) {
        append(lowerOut_, channel_->takeLowerOut());
        if (channel_->lowerDone()) lowerDone_ = true;
    }
    if (channel_ && channel_->ended()) {
        channel_.reset();
        session_.reset();
    }
}

void Engine::pumpSession(TimeMs now) {
    if (!session_ || !channel_) return;
    for (auto& e : session_->takeEvents()) {
        switch (e.kind) {
            case b2f::Event::Kind::Log: post(Event::Kind::Log, e.mid, std::move(e.text)); break;
            case b2f::Event::Kind::Handshake: post(Event::Kind::Log, {}, "gateway: " + e.text); break;
            case b2f::Event::Kind::Received: {
                // Already stored by the session's store callback.
                ++received_;
                auto m = Message::parse(e.data);
                post(Event::Kind::Received, e.mid, m ? m->subject() : decodeHeaderWords(e.text));
                if (m) {
                    if (auto pw = accountActivationPassword(*m)) post(Event::Kind::AccountActivated, e.mid, *pw);
                }
                break;
            }
            case b2f::Event::Kind::Sent:
            case b2f::Event::Kind::Rejected:
                // Rejected means the gateway already has it: delivered either way.
                mailbox_.move(e.mid, Mailbox::Folder::Out, Mailbox::Folder::Sent);
                ++sent_;
                post(Event::Kind::Sent, e.mid);
                break;
            case b2f::Event::Kind::Deferred:
                post(Event::Kind::Log, e.mid, "gateway deferred " + e.mid + "; it stays in the outbox");
                break;
            case b2f::Event::Kind::Progress: {
                Event p;
                p.kind = Event::Kind::Progress;
                p.mid = e.mid;
                p.done = e.done;
                p.total = e.total;
                events_.push_back(std::move(p));
                break;
            }
            case b2f::Event::Kind::Done: break;
            case b2f::Event::Kind::Failed:
                if (failure_.empty()) failure_ = e.text;
                break;
        }
    }
    if (auto out = session_->takeOut(); !out.empty()) channel_->write(out, now);
    if (session_->finished() && !closing_) {
        closing_ = true;
        post(Event::Kind::Status, {}, "closing the link");
        channel_->close(now);
    }
}

void Engine::endSession(bool ok, std::string text) {
    if (ok) {
        text = "done: " + std::to_string(sent_) + " sent, " + std::to_string(received_) + " received";
    }
    Event e;
    e.kind = Event::Kind::SessionEnded;
    e.ok = ok;
    e.text = std::move(text);
    events_.push_back(std::move(e));
}

}  // namespace ogm
