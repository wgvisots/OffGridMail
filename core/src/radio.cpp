#include "ogm/radio.hpp"

#include <cctype>

#include "ogm/kiss.hpp"

namespace ogm::radio {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

}  // namespace

ThD75::ThD75() : ThD75(Config{}) {}
ThD75::ThD75(Config cfg) : cfg_(cfg), band_(cfg.band) {}

void ThD75::onLinkUp(TimeMs now) {
    bandTries_ = 0;
    band_ = cfg_.band;
    append(out_, kiss::exitSequence());
    sendText("\r");  // flush any half-typed command; the radio answers "?"
    sendText("ID\r");
    deadline_ = now + cfg_.probeTimeout;
    step_ = Step::Probing;
    set(State::Starting, "asking the radio who it is");
}

void ThD75::onText(std::string_view raw, TimeMs now) {
    std::string_view text = trim(raw);
    if (text.empty()) return;
    switch (step_) {
        case Step::Probing:
            if (text.substr(0, 2) == "ID") {
                id_ = std::string(trim(text.substr(2)));
                log("radio: " + std::string(text));
                sendText("TN 2," + std::to_string(band_) + "\r");
                deadline_ = now + 2 * cfg_.probeTimeout;
                step_ = Step::Entering;
                set(State::Starting, "switching the TNC to KISS on band " + std::string(band_ ? "B" : "A"));
            }
            break;
        case Step::Entering:
            if (text.substr(0, 4) == "TN 2") {
                sendText("TN\r");  // once in KISS, this gets no answer
                deadline_ = now + cfg_.probeTimeout;
                step_ = Step::Confirming;
            } else if (text == "N" && ++bandTries_ < 2) {
                band_ = 1 - band_;  // KISS only runs on the data band
                sendText("TN 2," + std::to_string(band_) + "\r");
                deadline_ = now + 2 * cfg_.probeTimeout;
            } else {
                step_ = Step::Idle;
                set(State::Failed, "the radio refused KISS mode (" + std::string(text) + ")");
            }
            break;
        case Step::Confirming:
            step_ = Step::Idle;
            set(State::Failed, "the radio is still answering commands, so KISS did not start");
            break;
        case Step::Idle:
            log("radio text: " + std::string(text));
            break;
    }
}

void ThD75::onFrameSeen(TimeMs) {
    if (state_ == State::Starting) {
        step_ = Step::Idle;
        set(State::Ready, "KISS ready");
    }
}

void ThD75::tick(TimeMs now) {
    if (step_ == Step::Idle || now < deadline_) return;
    if (step_ == Step::Confirming) {
        step_ = Step::Idle;
        set(State::Ready, "KISS ready");  // TN 2,x was echoed, now commands go unanswered
        return;
    }
    const char* why = step_ == Step::Probing ? "the radio does not answer (is another app holding its Bluetooth link?)"
                                             : "the radio did not answer the KISS command";
    step_ = Step::Idle;
    set(State::Failed, why);
}

Bytes ThD75::shutdownBytes() const { return kiss::exitSequence(); }

std::unique_ptr<KissTnc> makeKissTnc(std::string_view model) {
    std::string m;
    for (char c : model) m += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (m == "th-d75" || m == "thd75" || m == "d75" || m == "th-d74" || m == "d74") return std::make_unique<ThD75>();
    if (m == "kiss" || m == "direwolf" || m == "tnc") return std::make_unique<PlainKiss>();
    return nullptr;
}

}  // namespace ogm::radio
