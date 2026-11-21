// KISS framing (FEND/FESC) for the TH-D75's built-in TNC.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ogm/common.hpp"

namespace ogm::kiss {

inline constexpr std::uint8_t FEND = 0xC0;
inline constexpr std::uint8_t FESC = 0xDB;
inline constexpr std::uint8_t TFEND = 0xDC;
inline constexpr std::uint8_t TFESC = 0xDD;

// Wrap one AX.25 frame (no FCS; the TNC adds it) as a KISS data frame.
Bytes wrap(std::span<const std::uint8_t> ax25, std::uint8_t port = 0);

// C0 FF C0: tells the TNC to leave KISS mode.
Bytes exitSequence();

// KISS parameter commands (value in the units the KISS spec defines).
inline constexpr std::uint8_t kTxDelay = 0x01;     // 10 ms units
inline constexpr std::uint8_t kPersistence = 0x02; // p = (value + 1) / 256
inline constexpr std::uint8_t kSlotTime = 0x03;    // 10 ms units
Bytes parameter(std::uint8_t command, std::uint8_t value, std::uint8_t port = 0);

// Streaming decoder.  Feed it whatever the link delivers, in any chunk size.
//
// Frames may share a FEND ("C0 a C0 b C0").  Bytes outside any frame are
// reported as Text: when the radio is not in KISS mode, those are its replies
// to PC commands, which is how the app tells the two modes apart.
class Decoder {
public:
    struct Item {
        enum class Kind { Frame, Text };
        Kind kind;
        std::uint8_t command = 0;  // Frame only: high nibble port, low nibble command
        Bytes data;                // Frame: unescaped body after the command byte
    };

    // Frames longer than this are dropped as garbage.  Connected-mode I-frames
    // carry at most 256 bytes of data plus a 72-byte header.
    static constexpr std::size_t kMaxFrame = 1024;

    std::vector<Item> feed(std::span<const std::uint8_t> bytes);

    std::size_t droppedFrames() const { return dropped_; }

private:
    void emitText(std::vector<Item>& out);

    bool inFrame_ = false;
    bool escape_ = false;
    bool overflow_ = false;
    Bytes buf_;
    Bytes text_;
    std::size_t dropped_ = 0;
};

}  // namespace ogm::kiss
