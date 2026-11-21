#include "ogm/b2f.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "ogm/lzhuf.hpp"
#include "ogm/secure_login.hpp"

namespace ogm::b2f {
namespace {

constexpr std::uint8_t SOH = 0x01, STX = 0x02, EOT = 0x04, NUL = 0x00;
constexpr std::size_t kMaxProposals = 5;
constexpr std::size_t kMaxTitle = 80;

bool startsWith(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

std::vector<std::string_view> split(std::string_view s) {
    std::vector<std::string_view> out;
    while (!s.empty()) {
        auto sp = s.find(' ');
        if (sp != 0) out.push_back(s.substr(0, sp));
        if (sp == std::string_view::npos) break;
        s.remove_prefix(sp + 1);
    }
    return out;
}

std::size_t toSize(std::string_view s) {
    std::size_t v = 0;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return 0;
        v = v * 10 + static_cast<std::size_t>(c - '0');
    }
    return v;
}

}  // namespace

std::string formatSid(const std::string& name, const std::string& version) {
    return "[" + name + "-" + version + "-B2FHM$]";
}

unsigned proposalChecksum(const std::vector<std::string>& lines) {
    unsigned sum = 0;
    for (const auto& l : lines) {
        for (char c : l) sum += static_cast<unsigned char>(c);
        sum += '\r';
    }
    return (0x100 - (sum & 0xff)) & 0xff;
}

Session::Session(Config cfg, std::vector<Outgoing> outbound, std::function<bool(const std::string&)> haveMid)
    : cfg_(std::move(cfg)), outgoing_(std::move(outbound)), haveMid_(std::move(haveMid)) {
    if (!haveMid_) haveMid_ = [](const std::string&) { return false; };
}

void Session::post(Event::Kind k, std::string mid, std::string text) {
    Event e;
    e.kind = k;
    e.mid = std::move(mid);
    e.text = std::move(text);
    events_.push_back(std::move(e));
}

void Session::sendLine(std::string_view text) {
    append(out_, text);
    out_.push_back('\r');
    post(Event::Kind::Log, {}, ">" + std::string(text));
}

Bytes Session::takeOut() { return std::exchange(out_, {}); }
std::vector<Event> Session::takeEvents() { return std::exchange(events_, {}); }

void Session::fail(std::string why) {
    if (finished()) return;
    state_ = State::Failed;
    post(Event::Kind::Failed, {}, std::move(why));
}

void Session::finish() {
    if (finished()) return;
    state_ = State::Done;
    post(Event::Kind::Done);
}

void Session::start() {
    if (!cfg_.master) return;
    // Act like a CMS: SID, optional secure-login challenge, then a prompt.
    sendLine("[WL2K-5.0-B2FWIHJM$]");
    if (!cfg_.challenge.empty()) sendLine(";PQ: " + cfg_.challenge);
    sendLine(cfg_.mycall + " CMS>");
}

void Session::onData(ByteView bytes) {
    for (std::size_t i = 0; i < bytes.size() && !finished(); ++i) {
        std::uint8_t b = bytes[i];
        if (state_ == State::Receiving) {
            binaryByte(b);
            continue;
        }
        if (b == '\r' || b == '\n') {
            if (!lineBuf_.empty()) line(std::exchange(lineBuf_, {}));
            continue;
        }
        lineBuf_ += static_cast<char>(b);
        if (lineBuf_.size() > 1024) {
            fail("protocol line too long");
            return;
        }
    }
}

void Session::line(std::string_view text) {
    post(Event::Kind::Log, {}, "<" + std::string(text));
    // "*** ..." is how gateways report errors (bad password, etc.).
    if (startsWith(text, "***")) {
        if (state_ == State::Handshake && text.find("MTD Stats") != std::string_view::npos) return;
        auto star = text.find_last_of('*');
        std::string why(text.substr(star + 1));
        while (!why.empty() && why.front() == ' ') why.erase(0, 1);
        fail(why.empty() ? std::string(text) : why);
        return;
    }
    if (startsWith(text, ";PM: ")) {
        auto f = split(text.substr(5));
        if (f.size() >= 4) {
            Pending p;
            p.to = f[0];
            p.mid = f[1];
            p.size = toSize(f[2]);
            p.from = f[3];
            auto subj = text.substr(5);
            for (int k = 0; k < 4; ++k) subj.remove_prefix(std::min(subj.size(), subj.find(' ') + 1));
            p.subject = subj;
            pending_[p.mid] = p;
        }
        return;
    }
    switch (state_) {
        case State::Handshake: handshakeLine(text); break;
        case State::AwaitAnswer:
            if (text[0] == ';') return;
            if (!startsWith(text, "FS ")) {
                fail("expected a proposal answer, got: " + std::string(text));
                return;
            }
            {
                std::string_view ans = text.substr(3);
                std::size_t i = 0;
                for (auto& p : sentBlock_) {
                    if (i >= ans.size()) {
                        fail("proposal answer is too short");
                        return;
                    }
                    char c = ans[i++];
                    switch (c) {
                        case 'Y': case 'y': case '+': p.answer = '+'; break;
                        case 'N': case 'n': case 'R': case 'r': case '-': p.answer = '-'; break;
                        case 'L': case 'l': case '=': case 'H': case 'h': p.answer = '='; break;
                        case 'A': case 'a': case '!': {
                            // "Accept from offset N" (resume).  We always send whole
                            // messages: offset 0 is a plain accept, anything else waits
                            // for a later session.
                            std::size_t start = i;
                            while (i < ans.size() && std::isdigit(static_cast<unsigned char>(ans[i]))) ++i;
                            if (toSize(ans.substr(start, i - start)) != 0) {
                                p.answer = '=';
                            } else {
                                p.answer = '+';
                            }
                            break;
                        }
                        default: fail("bad character in proposal answer: " + std::string(text)); return;
                    }
                }
            }
            for (const auto& p : sentBlock_) {
                if (p.answer == '+') {
                    sendMessage(p);
                    awaitingConfirm_.push_back(p.mid);
                } else if (p.answer == '-') {
                    post(Event::Kind::Rejected, p.mid);
                } else {
                    post(Event::Kind::Deferred, p.mid);
                }
            }
            // Messages that were answered are out of this session's queue.
            outgoing_.erase(std::remove_if(outgoing_.begin(), outgoing_.end(),
                                           [&](const Outgoing& o) {
                                               return std::any_of(sentBlock_.begin(), sentBlock_.end(),
                                                                  [&](const Proposal& p) { return p.mid == o.mid; });
                                           }),
                            outgoing_.end());
            sentBlock_.clear();
            state_ = State::RemoteTurn;
            inBlock_.clear();
            inChecksum_ = 0;
            break;
        case State::RemoteTurn: remoteTurnLine(text); break;
        default: break;
    }
}

void Session::handshakeLine(std::string_view text) {
    if (text.front() == '[' && text.back() == ']') {
        remoteSid_ = std::string(text);
        // The features are after the last dash; B2 is required.
        auto dash = text.find_last_of('-');
        std::string feat(dash == std::string_view::npos ? text : text.substr(dash + 1));
        for (char& c : feat) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (feat.find("B2") == std::string::npos) {
            fail("remote does not support the B2 forwarding protocol: " + remoteSid_);
            return;
        }
        post(Event::Kind::Handshake, {}, remoteSid_);
        return;
    }
    if (startsWith(text, ";PQ: ")) {
        challengeSeen_ = std::string(text.substr(5));
        return;
    }
    if (startsWith(text, ";PR: ")) {
        if (cfg_.master && !cfg_.challenge.empty() && !cfg_.password.empty())
            gotResponse_ = text.substr(5) == secureLoginResponse(cfg_.challenge, cfg_.password);
        return;
    }
    if (cfg_.master) {
        // The client's handshake ends where its first turn begins.
        if (text.front() == 'F') {
            if (remoteSid_.empty()) {
                fail("client sent no SID");
                return;
            }
            if (!cfg_.challenge.empty() && !cfg_.password.empty() && !gotResponse_) {
                append(out_, std::string_view("*** Secure login failed - account password does not match\r"));
                fail("client failed secure login");
                return;
            }
            state_ = State::RemoteTurn;
            remoteTurnLine(text);
        }
        return;
    }
    if (text.back() == '>') {
        if (remoteSid_.empty()) {
            fail("gateway sent no SID before its prompt");
            return;
        }
        // A challenge with no password set: carry on without ";PR:".  For a
        // brand-new callsign that is how the account gets created; for an
        // existing one the gateway answers "*** Secure login failed", which
        // says more than a local guess would.
        if (!challengeSeen_.empty() && cfg_.password.empty())
            post(Event::Kind::Log, {}, "gateway asked for a password and none is set; continuing without");
        sendHandshake();
        ourTurn();
    }
}

void Session::sendHandshake() {
    sendLine(";FW: " + cfg_.mycall);
    sendLine(formatSid(cfg_.appName, cfg_.appVersion));
    if (!challengeSeen_.empty() && !cfg_.password.empty()) sendLine(";PR: " + secureLoginResponse(challengeSeen_, cfg_.password));
    std::string comment = "; " + cfg_.targetcall + " DE " + cfg_.mycall;
    if (!cfg_.locator.empty()) comment += " (" + cfg_.locator + ")";
    sendLine(comment);
}

void Session::ourTurn() {
    if (outgoing_.empty()) {
        if (remoteNoMsgs_) {
            sendLine("FQ");
            quit_ = true;
            finish();
        } else {
            sendLine("FF");
            state_ = State::RemoteTurn;
            inBlock_.clear();
            inChecksum_ = 0;
        }
        return;
    }
    std::vector<std::string> lines;
    for (std::size_t i = 0; i < outgoing_.size() && sentBlock_.size() < kMaxProposals; ++i) {
        const auto& o = outgoing_[i];
        Proposal p;
        p.mid = o.mid;
        p.size = o.raw.size();
        p.compressed = lzhuf::compress(o.raw);
        p.csize = p.compressed.size();
        p.title = o.title.empty() ? "No title" : o.title.substr(0, kMaxTitle);
        char buf[96];
        std::snprintf(buf, sizeof buf, "FC EM %s %zu %zu 0", p.mid.c_str(), p.size, p.csize);
        lines.emplace_back(buf);
        sentBlock_.push_back(std::move(p));
    }
    for (const auto& l : lines) sendLine(l);
    char sum[16];
    std::snprintf(sum, sizeof sum, "F> %02X", proposalChecksum(lines));
    sendLine(sum);
    state_ = State::AwaitAnswer;
}

void Session::sendMessage(const Proposal& p) {
    // SOH, header length, title NUL offset NUL
    std::string offset = "0";
    out_.push_back(SOH);
    out_.push_back(static_cast<std::uint8_t>(p.title.size() + offset.size() + 2));
    append(out_, p.title);
    out_.push_back(NUL);
    append(out_, offset);
    out_.push_back(NUL);
    unsigned sum = 0;
    for (std::size_t i = 0; i < p.compressed.size(); i += cfg_.blockSize) {
        std::size_t n = std::min(cfg_.blockSize, p.compressed.size() - i);
        out_.push_back(STX);
        out_.push_back(static_cast<std::uint8_t>(n & 0xff));  // 256 would be sent as 0
        for (std::size_t k = 0; k < n; ++k) {
            std::uint8_t b = p.compressed[i + k];
            out_.push_back(b);
            sum += b;
        }
    }
    out_.push_back(EOT);
    out_.push_back(static_cast<std::uint8_t>((0x100 - (sum & 0xff)) & 0xff));
    post(Event::Kind::Log, p.mid, ">[" + p.mid + ": " + std::to_string(p.csize) + " bytes]");
}

void Session::confirmSent() {
    for (const auto& mid : awaitingConfirm_) post(Event::Kind::Sent, mid);
    awaitingConfirm_.clear();
}

void Session::remoteTurnLine(std::string_view text) {
    // The remote's first line after our messages proves it took them.
    if (!awaitingConfirm_.empty() && (text[0] == 'F' || text[0] == ';')) confirmSent();
    if (text[0] == ';') return;
    if (text.size() < 2 || text[0] != 'F') {
        fail("unexpected protocol line: " + std::string(text));
        return;
    }
    switch (text[1]) {
        case 'A':
        case 'B':
        case 'C':
        case 'D': {
            for (char c : text) inChecksum_ += static_cast<unsigned char>(c);
            inChecksum_ += '\r';
            Proposal p;
            p.code = text[1];
            if (text.size() < 4) {
                fail("malformed proposal: " + std::string(text));
                return;
            }
            auto f = split(text.substr(3));
            if (f.size() < 4) {
                fail("malformed proposal: " + std::string(text));
                return;
            }
            p.type = std::string(f[0]);
            p.mid = std::string(f[1]);
            p.size = toSize(f[2]);
            p.csize = toSize(f[3]);
            inBlock_.push_back(std::move(p));
            break;
        }
        case 'F':
            remoteNoMsgs_ = true;
            ourTurn();
            break;
        case 'Q':
            quit_ = true;
            finish();
            break;
        case '>': {
            unsigned want = (0x100 - (inChecksum_ & 0xff)) & 0xff;
            unsigned got = 0;
            auto hex = text.substr(2);
            while (!hex.empty() && hex.front() == ' ') hex.remove_prefix(1);
            if (std::sscanf(std::string(hex).c_str(), "%x", &got) != 1 || got != want) {
                fail("proposal checksum mismatch");
                return;
            }
            if (inBlock_.empty()) {
                remoteNoMsgs_ = true;
                ourTurn();
                return;
            }
            remoteNoMsgs_ = false;
            answerProposals();
            break;
        }
        default: fail("unknown protocol command: " + std::string(text));
    }
}

void Session::answerProposals() {
    std::string answers;
    std::vector<std::string> seen;
    for (auto& p : inBlock_) {
        bool dup = std::find(seen.begin(), seen.end(), p.mid) != seen.end();
        seen.push_back(p.mid);
        if (dup || p.code != 'C' || (p.type != "EM" && p.type != "CM") || p.csize < 6) {
            p.answer = '=';
        } else if (haveMid_(p.mid)) {
            p.answer = '-';
        } else {
            p.answer = '+';
        }
        answers += p.answer;
    }
    sendLine("FS " + answers);
    inBlock_.erase(std::remove_if(inBlock_.begin(), inBlock_.end(), [](const Proposal& p) { return p.answer != '+'; }),
                   inBlock_.end());
    if (inBlock_.empty()) {
        ourTurn();
        return;
    }
    recvIndex_ = 0;
    bin_ = Bin::Soh;
    state_ = State::Receiving;
}

void Session::binaryByte(std::uint8_t b) {
    Proposal& p = inBlock_[recvIndex_];
    switch (bin_) {
        case Bin::Soh:
            if (b == SOH) {
                bin_ = Bin::HeaderLen;
            } else if (b == '*') {
                // An error line instead of a message.
                state_ = State::RemoteTurn;
                lineBuf_ = "*";
            } else {
                fail("expected the start of a message, got byte " + std::to_string(b));
            }
            return;
        case Bin::HeaderLen:
            binHeaderLen_ = b;
            binTitle_.clear();
            binOffset_.clear();
            bin_ = Bin::Title;
            return;
        case Bin::Title:
            if (b == NUL) {
                bin_ = Bin::Offset;
            } else if (binTitle_.size() < 255) {
                binTitle_ += static_cast<char>(b);
            }
            return;
        case Bin::Offset:
            if (b != NUL) {
                binOffset_ += static_cast<char>(b);
                return;
            }
            if (binTitle_.size() + binOffset_.size() + 2 != binHeaderLen_) {
                fail("message header length mismatch");
                return;
            }
            if (binOffset_ != "0") {
                fail("remote resumed at offset " + binOffset_ + ", which was not asked for");
                return;
            }
            p.title = binTitle_;
            binData_.clear();
            binSum_ = 0;
            bin_ = Bin::Type;
            return;
        case Bin::Type:
            if (b == STX) {
                bin_ = Bin::Len;
            } else if (b == EOT) {
                bin_ = Bin::Checksum;
            } else {
                fail("unexpected byte in message data: " + std::to_string(b));
            }
            return;
        case Bin::Len:
            binRemain_ = b == 0 ? 256 : b;
            bin_ = Bin::Data;
            return;
        case Bin::Data:
            binData_.push_back(b);
            binSum_ += b;
            if (binData_.size() > p.csize) {
                fail("message " + p.mid + " is longer than proposed");
                return;
            }
            if (--binRemain_ == 0) {
                bin_ = Bin::Type;
                Event e;
                e.kind = Event::Kind::Progress;
                e.mid = p.mid;
                e.done = binData_.size();
                e.total = p.csize;
                events_.push_back(std::move(e));
            }
            return;
        case Bin::Checksum:
            if (((binSum_ + b) & 0xff) != 0) {
                fail("message " + p.mid + " has a bad checksum");
                return;
            }
            if (binData_.size() != p.csize) {
                fail("message " + p.mid + " has the wrong length");
                return;
            }
            finishReceived();
            return;
    }
}

void Session::finishReceived() {
    Proposal& p = inBlock_[recvIndex_];
    std::string err;
    auto plain = lzhuf::decompress(binData_, true, &err);
    if (!plain) {
        fail("message " + p.mid + " does not decompress: " + err);
        return;
    }
    if (store_) {
        std::string why;
        if (!store_(p.mid, *plain, why)) {
            // Say so before the remote takes our next turn as an acknowledgement.
            append(out_, "*** " + (why.empty() ? std::string("cannot store message ") + p.mid : why) + "\r");
            fail("could not store message " + p.mid + (why.empty() ? "" : ": " + why));
            return;
        }
    }
    Event e;
    e.kind = Event::Kind::Received;
    e.mid = p.mid;
    e.text = p.title;
    e.data = std::move(*plain);
    events_.push_back(std::move(e));
    post(Event::Kind::Log, p.mid, "<[" + p.mid + ": " + std::to_string(p.csize) + " bytes]");
    if (++recvIndex_ < inBlock_.size()) {
        bin_ = Bin::Soh;
        return;
    }
    inBlock_.clear();
    inChecksum_ = 0;
    state_ = State::RemoteTurn;  // ourTurn() may change it again
    ourTurn();
}

}  // namespace ogm::b2f
