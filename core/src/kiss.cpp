#include "ogm/kiss.hpp"

namespace ogm::kiss {

Bytes wrap(std::span<const std::uint8_t> ax25, std::uint8_t port) {
    Bytes out;
    out.reserve(ax25.size() + 8);
    out.push_back(FEND);
    out.push_back(static_cast<std::uint8_t>((port & 0x0F) << 4));
    for (std::uint8_t b : ax25) {
        if (b == FEND) {
            out.push_back(FESC);
            out.push_back(TFEND);
        } else if (b == FESC) {
            out.push_back(FESC);
            out.push_back(TFESC);
        } else {
            out.push_back(b);
        }
    }
    out.push_back(FEND);
    return out;
}

Bytes exitSequence() { return {FEND, 0xFF, FEND}; }

Bytes parameter(std::uint8_t command, std::uint8_t value, std::uint8_t port) {
    std::uint8_t cmd = static_cast<std::uint8_t>(((port & 0x0F) << 4) | (command & 0x0F));
    Bytes out{FEND, cmd};
    if (value == FEND) {
        out.push_back(FESC);
        out.push_back(TFEND);
    } else if (value == FESC) {
        out.push_back(FESC);
        out.push_back(TFESC);
    } else {
        out.push_back(value);
    }
    out.push_back(FEND);
    return out;
}

void Decoder::emitText(std::vector<Item>& out) {
    if (!text_.empty()) {
        out.push_back({Item::Kind::Text, 0, std::move(text_)});
        text_.clear();
    }
}

std::vector<Decoder::Item> Decoder::feed(std::span<const std::uint8_t> bytes) {
    std::vector<Item> out;
    for (std::uint8_t b : bytes) {
        if (b == FEND) {
            emitText(out);
            if (inFrame_ && !buf_.empty()) {
                // Closing FEND.  The next byte decides whether a shared-FEND
                // frame follows (a KISS command byte) or text does.
                if (overflow_) {
                    ++dropped_;
                } else {
                    Item it{Item::Kind::Frame, buf_.front(), Bytes(buf_.begin() + 1, buf_.end())};
                    out.push_back(std::move(it));
                }
                inFrame_ = false;
            } else {
                inFrame_ = true;
            }
            buf_.clear();
            escape_ = false;
            overflow_ = false;
            continue;
        }
        if (!inFrame_) {
            // A KISS command byte (data 0x00, parameters 0x01-0x06, return
            // 0xFF) opens a shared-FEND frame; anything else, CR and LF
            // included, is the radio talking in its command mode.
            if (text_.empty() && (b <= 0x06 || b == 0xFF)) {
                inFrame_ = true;
                buf_.push_back(b);
            } else {
                text_.push_back(b);
                if (b == '\r' || b == '\n' || text_.size() >= kMaxFrame) emitText(out);
            }
            continue;
        }
        if (escape_) {
            b = (b == TFEND) ? FEND : (b == TFESC) ? FESC : b;
            escape_ = false;
        } else if (b == FESC) {
            escape_ = true;
            continue;
        }
        if (buf_.size() < kMaxFrame + 1) {
            buf_.push_back(b);
        } else {
            overflow_ = true;
        }
    }
    return out;
}

}  // namespace ogm::kiss
