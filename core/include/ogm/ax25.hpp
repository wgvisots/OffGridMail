// AX.25 frames: addresses, digipeater path, control, PID, info.
//
// Carried over from aprs_sms and extended for connected mode: the info field
// is binary, and the command/response bits in the address SSID bytes are
// encoded and decoded (AX.25 v2.0).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ogm/common.hpp"

namespace ogm::ax25 {

struct Address {
    std::string call;        // 1-6 upper-case letters/digits
    std::uint8_t ssid = 0;   // 0-15
    bool hBit = false;       // "has been repeated" (path entries only)

    // "W1ABC-7", or "WIDE1-1*" when the H bit is set and withStar is true.
    std::string str(bool withStar = true) const;

    // Parses "CALL", "CALL-SSID", optionally with a trailing '*'.
    static std::optional<Address> parse(std::string_view text);

    bool sameStation(const Address& o) const { return call == o.call && ssid == o.ssid; }
};

inline constexpr std::uint8_t kControlUi = 0x03;
inline constexpr std::uint8_t kPidNoLayer3 = 0xF0;
inline constexpr std::size_t kMaxDigis = 8;

// The C bits of AX.25 v2.0 (destination, source): Command is (1,0) and
// Response is (0,1).  Version 1 frames have both bits equal; both forms are
// kept so a decoded frame re-encodes to the same bytes.
enum class Cr { Legacy, Command, Response, LegacySet };

struct Frame {
    Address dest;
    Address src;
    std::vector<Address> path;
    Cr cr = Cr::Legacy;
    std::uint8_t control = kControlUi;
    std::optional<std::uint8_t> pid = kPidNoLayer3;  // present on I and UI frames only
    Bytes info;

    // True when some digipeater has already repeated this frame.
    bool wasDigipeated() const;
    // True when every digipeater in the path has repeated it (or there is no path).
    bool fullyRepeated() const;

    // TNC2 monitor format: SRC>DEST,PATH*:info (non-printables shown as '.').
    std::string tnc2() const;
};

// Encode a frame.  Returns nullopt if an address is invalid or the path is
// longer than kMaxDigis.
std::optional<Bytes> encode(const Frame& f);

// Decode a frame received from the TNC (without FCS).  On failure returns
// nullopt and, if error is non-null, a short reason.
std::optional<Frame> decode(ByteView bytes, std::string* error = nullptr);

// Parse "WIDE1-1,WIDE2-1" into addresses (no stars).  Empty text is an empty path.
std::optional<std::vector<Address>> parsePath(std::string_view text);

// Does this control byte carry a PID (I and UI frames)?
bool hasPid(std::uint8_t control);

}  // namespace ogm::ax25
