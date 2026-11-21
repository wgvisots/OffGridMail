#include "fake_rms.hpp"

#include "ogm/message.hpp"

namespace ogm::sim {

namespace {
ax25::LinkConfig linkConfig(const std::string& call) {
    ax25::LinkConfig c;
    c.mycall = ax25::Address::parse(call).value_or(ax25::Address{"N0CALL", 10, false});
    c.acceptIncoming = true;
    return c;
}
}  // namespace

FakeRms::FakeRms(Config cfg, Mailbox& mailbox) : cfg_(std::move(cfg)), mailbox_(mailbox), link_(linkConfig(cfg_.call)) {}

void FakeRms::onClientConnected(TimeMs) {
    decoder_ = kiss::Decoder{};
    hangup_ = false;
    closing_ = false;
    session_.reset();
    if (cfg_.telnet) {
        append(out_, std::string_view("Callsign :\r"));
        loginStep_ = 1;
        lineBuf_.clear();
    }
}

void FakeRms::startSession(TimeMs now) {
    std::vector<b2f::Outgoing> out;
    for (const auto& mid : mailbox_.list(Mailbox::Folder::Out)) {
        auto raw = mailbox_.load(Mailbox::Folder::Out, mid);
        if (!raw) continue;
        auto m = Message::parse(*raw);
        if (m) out.push_back({mid, m->header("Subject"), std::move(*raw)});
    }
    b2f::Config bc;
    bc.mycall = cfg_.call;
    bc.master = true;
    bc.password = cfg_.password;
    bc.challenge = cfg_.password.empty() ? std::string() : cfg_.challenge;
    session_ = std::make_unique<b2f::Session>(bc, std::move(out), [this](const std::string& mid) { return mailbox_.has(mid); });
    session_->start();
    ++sessions_;
    log_.push_back("session " + std::to_string(sessions_) + " started");
    pumpSession(now);
}

void FakeRms::pumpSession(TimeMs now) {
    if (!session_) return;
    for (auto& e : session_->takeEvents()) {
        switch (e.kind) {
            case b2f::Event::Kind::Log: log_.push_back("b2f " + e.text); break;
            case b2f::Event::Kind::Received:
                mailbox_.save(Mailbox::Folder::In, e.mid, e.data);
                received_.push_back(e.mid);
                log_.push_back("received " + e.mid);
                break;
            case b2f::Event::Kind::Sent:
            case b2f::Event::Kind::Rejected:
                mailbox_.move(e.mid, Mailbox::Folder::Out, Mailbox::Folder::Sent);
                log_.push_back("delivered " + e.mid);
                break;
            case b2f::Event::Kind::Failed: log_.push_back("session failed: " + e.text); break;
            case b2f::Event::Kind::Done: log_.push_back("session done"); break;
            default: break;
        }
    }
    Bytes o = session_->takeOut();
    if (!o.empty()) {
        if (cfg_.telnet) {
            append(out_, o);
        } else {
            link_.write(o, now);
        }
    }
    if (session_->finished() && !closing_) {
        closing_ = true;
        // A CMS hangs up after FQ; it does the same after a failed login.
        if (cfg_.telnet) {
            hangup_ = true;
        } else {
            link_.disconnect(now);
        }
    }
}

void FakeRms::pumpLink(TimeMs now) {
    for (auto& f : link_.takeOutgoing()) {
        if (cfg_.dropEvery > 0 && ++frameCount_ % cfg_.dropEvery == 0) {
            log_.push_back("(dropped) " + f.tnc2());
            continue;
        }
        if (auto b = ax25::encode(f)) append(out_, kiss::wrap(*b));
    }
    for (auto& e : link_.takeEvents()) {
        switch (e.kind) {
            case ax25::Link::Event::Kind::Connected:
                log_.push_back("link up with " + link_.remote().str(false));
                closing_ = false;
                startSession(now);
                break;
            case ax25::Link::Event::Kind::Data:
                if (session_) session_->onData(e.data);
                break;
            case ax25::Link::Event::Kind::Disconnected:
                log_.push_back("link down" + (e.text.empty() ? std::string() : ": " + e.text));
                session_.reset();
                break;
            case ax25::Link::Event::Kind::Log: log_.push_back("link " + e.text); break;
        }
    }
    pumpSession(now);
    for (auto& f : link_.takeOutgoing())
        if (auto b = ax25::encode(f)) append(out_, kiss::wrap(*b));
}

void FakeRms::onBytes(ByteView bytes, TimeMs now) {
    if (cfg_.telnet) {
        std::size_t i = 0;
        while (loginStep_ < 3 && i < bytes.size()) {
            char c = static_cast<char>(bytes[i++]);
            if (c != '\r' && c != '\n') {
                lineBuf_ += c;
                continue;
            }
            if (lineBuf_.empty()) continue;
            if (loginStep_ == 1) {
                log_.push_back("telnet login from " + lineBuf_);
                append(out_, std::string_view("Password :\r"));
                loginStep_ = 2;
            } else if (loginStep_ == 2) {
                loginStep_ = 3;
                startSession(now);
            }
            lineBuf_.clear();
        }
        if (loginStep_ == 3 && i < bytes.size() && session_) {
            session_->onData(bytes.subspan(i));
            pumpSession(now);
        }
        return;
    }
    for (auto& item : decoder_.feed(bytes)) {
        if (item.kind != kiss::Decoder::Item::Kind::Frame || (item.command & 0x0F) != 0) continue;
        if (auto f = ax25::decode(item.data)) link_.onFrame(*f, now);
    }
    pumpLink(now);
}

void FakeRms::tick(TimeMs now) {
    if (cfg_.telnet) return;
    link_.tick(now);
    pumpLink(now);
}

}  // namespace ogm::sim
