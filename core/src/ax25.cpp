#include "ogm/ax25.hpp"

#include <cctype>

namespace ogm::ax25 {
namespace {

bool validCall(std::string_view c) {
    if (c.empty() || c.size() > 6) return false;
    for (char ch : c) {
        if (!std::isupper(static_cast<unsigned char>(ch)) &&
            !std::isdigit(static_cast<unsigned char>(ch)))
            return false;
    }
    return true;
}

// bit7 is the H bit for path entries and the C bit for destination/source.
void putAddress(Bytes& out, const Address& a, bool bit7, bool last) {
    for (std::size_t i = 0; i < 6; ++i) {
        char ch = i < a.call.size() ? a.call[i] : ' ';
        out.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(ch) << 1));
    }
    std::uint8_t s = static_cast<std::uint8_t>(0x60 | ((a.ssid & 0x0F) << 1));
    if (bit7) s |= 0x80;
    if (last) s |= 0x01;
    out.push_back(s);
}

}  // namespace

bool Address::valid() const { return validCall(call) && ssid <= 15; }

bool hasPid(std::uint8_t control) {
    if ((control & 0x01) == 0) return true;               // I frame
    return (control & 0xEF) == kControlUi;                // UI, with or without P/F
}

std::string Address::str(bool withStar) const {
    std::string s = call;
    if (ssid != 0) s += "-" + std::to_string(ssid);
    if (withStar && hBit) s += '*';
    return s;
}

std::optional<Address> Address::parse(std::string_view text) {
    Address a;
    if (!text.empty() && text.back() == '*') {
        a.hBit = true;
        text.remove_suffix(1);
    }
    std::string_view base = text;
    if (auto dash = text.find('-'); dash != std::string_view::npos) {
        base = text.substr(0, dash);
        std::string_view num = text.substr(dash + 1);
        if (num.empty() || num.size() > 2) return std::nullopt;
        int v = 0;
        for (char ch : num) {
            if (!std::isdigit(static_cast<unsigned char>(ch))) return std::nullopt;
            v = v * 10 + (ch - '0');
        }
        if (v > 15) return std::nullopt;
        a.ssid = static_cast<std::uint8_t>(v);
    }
    a.call.reserve(base.size());
    for (char ch : base) a.call += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (!validCall(a.call)) return std::nullopt;
    return a;
}

bool Frame::wasDigipeated() const {
    for (const auto& p : path)
        if (p.hBit) return true;
    return false;
}

bool Frame::fullyRepeated() const { return path.empty() || path.back().hBit; }

std::string Frame::tnc2() const {
    std::string s = src.str(false) + ">" + dest.str(false);
    for (const auto& p : path) s += "," + p.str(true);
    s += ':';
    for (std::uint8_t b : info) s += (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.';
    return s;
}

std::optional<Bytes> encode(const Frame& f) {
    if (!validCall(f.dest.call) || !validCall(f.src.call) || f.path.size() > kMaxDigis)
        return std::nullopt;
    for (const auto& p : f.path)
        if (!validCall(p.call)) return std::nullopt;
    if (f.dest.ssid > 15 || f.src.ssid > 15) return std::nullopt;
    if (hasPid(f.control) != f.pid.has_value()) return std::nullopt;

    Bytes out;
    out.reserve(7 * (2 + f.path.size()) + 2 + f.info.size());
    putAddress(out, f.dest, f.cr == Cr::Command || f.cr == Cr::LegacySet, false);
    putAddress(out, f.src, f.cr == Cr::Response || f.cr == Cr::LegacySet, f.path.empty());
    for (std::size_t i = 0; i < f.path.size(); ++i)
        putAddress(out, f.path[i], f.path[i].hBit, i + 1 == f.path.size());
    out.push_back(f.control);
    if (f.pid) out.push_back(*f.pid);
    out.insert(out.end(), f.info.begin(), f.info.end());
    return out;
}

std::optional<Frame> decode(ByteView b, std::string* error) {
    auto fail = [&](const char* why) -> std::optional<Frame> {
        if (error) *error = why;
        return std::nullopt;
    };
    std::vector<Address> addrs;
    std::size_t i = 0;
    for (;;) {
        if (i + 7 > b.size()) return fail("address field runs off the end");
        Address a;
        for (std::size_t k = 0; k < 6; ++k) {
            std::uint8_t raw = b[i + k];
            if (raw & 0x01) return fail("extension bit set inside callsign");
            char ch = static_cast<char>(raw >> 1);
            if (ch != ' ') a.call += ch;
        }
        std::uint8_t s = b[i + 6];
        a.ssid = (s >> 1) & 0x0F;
        a.hBit = (s & 0x80) != 0;
        if (a.call.empty()) return fail("empty callsign");
        addrs.push_back(std::move(a));
        i += 7;
        if (s & 0x01) break;
        if (addrs.size() > 2 + kMaxDigis) return fail("too many addresses");
    }
    if (addrs.size() < 2) return fail("fewer than two addresses");
    if (i + 1 > b.size()) return fail("missing control field");

    Frame f;
    bool destC = addrs[0].hBit, srcC = addrs[1].hBit;
    f.cr = destC == srcC ? (destC ? Cr::LegacySet : Cr::Legacy) : destC ? Cr::Command : Cr::Response;
    f.dest = std::move(addrs[0]);
    f.src = std::move(addrs[1]);
    f.dest.hBit = f.src.hBit = false;
    f.path.assign(std::make_move_iterator(addrs.begin() + 2), std::make_move_iterator(addrs.end()));
    f.control = b[i++];
    if (hasPid(f.control)) {
        if (i + 1 > b.size()) return fail("missing PID");
        f.pid = b[i++];
    } else {
        f.pid.reset();
    }
    f.info.assign(b.begin() + static_cast<std::ptrdiff_t>(i), b.end());
    return f;
}

std::optional<std::vector<Address>> parsePath(std::string_view text) {
    std::vector<Address> out;
    while (!text.empty()) {
        auto comma = text.find(',');
        auto part = text.substr(0, comma);
        while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
        while (!part.empty() && part.back() == ' ') part.remove_suffix(1);
        auto a = Address::parse(part);
        if (!a) return std::nullopt;
        a->hBit = false;
        out.push_back(*a);
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    if (out.size() > kMaxDigis) return std::nullopt;
    return out;
}

}  // namespace ogm::ax25
