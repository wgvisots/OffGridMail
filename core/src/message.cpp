#include "ogm/message.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "ogm/md5.hpp"

namespace ogm {
namespace {

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}

// Days from 1970-01-01 to y-m-d, and back (Howard Hinnant's algorithms).
std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civilFromDays(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}

// Decode one UTF-8 code point at s[i]; returns 0xFFFD and advances one byte
// on malformed input.
char32_t nextCodePoint(std::string_view s, std::size_t& i) {
    auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    unsigned char c = b(i);
    if (c < 0x80) {
        ++i;
        return c;
    }
    int n = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (n < 0 || i + static_cast<std::size_t>(n) >= s.size()) {
        ++i;
        return 0xFFFD;
    }
    char32_t cp = c & (0x3F >> n);
    for (int k = 1; k <= n; ++k) {
        unsigned char cc = b(i + static_cast<std::size_t>(k));
        if ((cc & 0xC0) != 0x80) {
            ++i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (cc & 0x3F);
    }
    i += static_cast<std::size_t>(n) + 1;
    return cp;
}

void putUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Text in an unknown 8-bit charset: UTF-8 if it parses as UTF-8, else Latin-1.
std::string toUtf8Guess(std::string_view s, std::string_view charset) {
    std::string cs = upper(charset);
    if (cs == "UTF-8" || cs == "UTF8") return std::string(s);
    if (isValidUtf8(s)) return std::string(s);  // CMS sometimes forwards UTF-8 regardless
    return latin1ToUtf8(s);
}

std::string charsetOf(const Message& m) {
    std::string ct = m.header("Content-Type");
    std::string low;
    for (char c : ct) low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto pos = low.find("charset=");
    if (pos == std::string::npos) return "ISO-8859-1";
    std::string cs = ct.substr(pos + 8);
    if (!cs.empty() && cs.front() == '"') cs.erase(0, 1);
    auto end = cs.find_first_of("\"; ");
    if (end != std::string::npos) cs.resize(end);
    return cs.empty() ? "ISO-8859-1" : cs;
}

// A header value as stored: control characters (a stray CR, NUL...) become
// spaces, then the ends are trimmed, so writing it out and reading it back
// gives the same value.
std::string cleanValue(std::string_view v) {
    std::string out(v);
    for (char& c : out)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = ' ';
    return std::string(trim(out));
}

std::string crlf(std::string_view text) {
    std::string out;
    out.reserve(text.size() + text.size() / 32);
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r') {
            out += "\r\n";
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else if (c == '\n') {
            out += "\r\n";
        } else {
            out += c;
        }
    }
    return out;
}

}  // namespace

// ---- character sets

bool isValidUtf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        std::size_t before = i;
        if (nextCodePoint(s, i) == 0xFFFD && i == before + 1) return false;
    }
    return true;
}

std::string utf8ToLatin1(std::string_view s, bool* changed) {
    std::string out;
    out.reserve(s.size());
    bool ch = false;
    for (std::size_t i = 0; i < s.size();) {
        char32_t cp = nextCodePoint(s, i);
        if (cp < 0x100 && cp != 0xFFFD) {
            out += static_cast<char>(cp);
            continue;
        }
        ch = true;
        switch (cp) {
            case U'‘': case U'’': case U'‚': case U'′': out += '\''; break;
            case U'“': case U'”': case U'„': case U'″': out += '"'; break;
            case U'–': case U'—': case U'−': out += '-'; break;
            case U'…': out += "..."; break;
            case U' ': case U' ': case U' ': out += ' '; break;
            case U'•': out += '*'; break;
            case U'€': out += "EUR"; break;
            default: out += '?'; break;
        }
    }
    if (changed) *changed = ch;
    return out;
}

std::string latin1ToUtf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) putUtf8(out, static_cast<unsigned char>(c));
    return out;
}

std::string encodeHeaderWord(std::string_view utf8) {
    bool ascii = std::all_of(utf8.begin(), utf8.end(),
                             [](char c) { return static_cast<unsigned char>(c) < 0x80 && c != '\r' && c != '\n'; });
    if (ascii) return std::string(utf8);
    std::string latin = utf8ToLatin1(utf8);
    std::string out = "=?ISO-8859-1?Q?";
    static const char* hex = "0123456789ABCDEF";
    for (char c : latin) {
        auto u = static_cast<unsigned char>(c);
        if (u == ' ') {
            out += '_';
        } else if (u < 0x80 && std::isalnum(u)) {
            out += c;
        } else if (u < 0x80 && std::string_view("!*+-/").find(c) != std::string_view::npos) {
            out += c;
        } else {
            out += '=';
            out += hex[u >> 4];
            out += hex[u & 15];
        }
    }
    out += "?=";
    return out;
}

std::string decodeHeaderWords(std::string_view raw) {
    std::string out;
    std::size_t i = 0;
    bool lastWasWord = false;
    while (i < raw.size()) {
        auto start = raw.find("=?", i);
        if (start == std::string_view::npos) {
            out += toUtf8Guess(raw.substr(i), "");
            break;
        }
        // Encoded words: =?charset?enc?text?=
        auto q1 = raw.find('?', start + 2);
        auto q2 = q1 == std::string_view::npos ? q1 : raw.find('?', q1 + 1);
        auto end = q2 == std::string_view::npos ? q2 : raw.find("?=", q2 + 1);
        if (end == std::string_view::npos) {
            out += toUtf8Guess(raw.substr(i), "");
            break;
        }
        std::string_view between = raw.substr(i, start - i);
        // Whitespace between two encoded words is dropped (RFC 2047 6.2).
        if (!(lastWasWord && trim(between).empty())) out += toUtf8Guess(between, "");
        std::string_view cs = raw.substr(start + 2, q1 - start - 2);
        char enc = static_cast<char>(std::toupper(static_cast<unsigned char>(raw[q1 + 1])));
        std::string_view text = raw.substr(q2 + 1, end - q2 - 1);
        std::string bytes;
        if (enc == 'Q') {
            for (std::size_t k = 0; k < text.size(); ++k) {
                char c = text[k];
                if (c == '_') {
                    bytes += ' ';
                } else if (c == '=' && k + 2 < text.size() &&
                           std::isxdigit(static_cast<unsigned char>(text[k + 1])) &&
                           std::isxdigit(static_cast<unsigned char>(text[k + 2]))) {
                    bytes += static_cast<char>(std::stoi(std::string(text.substr(k + 1, 2)), nullptr, 16));
                    k += 2;
                } else {
                    bytes += c;
                }
            }
        } else if (enc == 'B') {
            static const std::string_view b64 =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            unsigned acc = 0;
            int nbits = 0;
            for (char c : text) {
                auto p = b64.find(c);
                if (p == std::string_view::npos) continue;
                acc = (acc << 6) | static_cast<unsigned>(p);
                nbits += 6;
                if (nbits >= 8) {
                    nbits -= 8;
                    bytes += static_cast<char>((acc >> nbits) & 0xFF);
                }
            }
        } else {
            bytes = std::string(text);
        }
        out += toUtf8Guess(bytes, cs);
        i = end + 2;
        lastWasWord = true;
    }
    return out;
}

// ---- addresses, MIDs, dates

std::string normalizeAddress(std::string_view addr) {
    addr = trim(addr);
    if (addr.size() > 5 && iequals(addr.substr(0, 5), "SMTP:")) return "SMTP:" + std::string(addr.substr(5));
    auto at = addr.find('@');
    if (at == std::string_view::npos) return upper(addr);
    if (iequals(addr.substr(at + 1), "winlink.org")) return upper(addr.substr(0, at));
    return "SMTP:" + std::string(addr);
}

std::string generateMid(std::string_view callsign, std::int64_t unixSeconds, std::uint64_t salt) {
    std::string payload = std::to_string(unixSeconds) + "-" + std::to_string(salt) + "-" + upper(callsign);
    auto sum = md5(asBytes(payload));
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string out;
    unsigned acc = 0;
    int nbits = 0;
    for (std::uint8_t b : sum) {
        acc = (acc << 8) | b;
        nbits += 8;
        while (nbits >= 5 && out.size() < 12) {
            nbits -= 5;
            out += alphabet[(acc >> nbits) & 31];
        }
        if (out.size() >= 12) break;
    }
    return out;
}

std::string formatDate(std::int64_t unixSeconds) {
    std::int64_t days = unixSeconds >= 0 ? unixSeconds / 86400 : (unixSeconds - 86399) / 86400;
    std::int64_t secs = unixSeconds - days * 86400;
    std::int64_t y;
    unsigned m, d;
    civilFromDays(days, y, m, d);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04lld/%02u/%02u %02lld:%02lld", static_cast<long long>(y), m, d,
                  static_cast<long long>(secs / 3600), static_cast<long long>((secs / 60) % 60));
    return buf;
}

std::optional<std::int64_t> parseDate(std::string_view text) {
    // Accept YYYY/MM/DD HH:MM, and the '.', '-' variants and BPQ's YYYYMMDDHHMMSS.
    text = trim(text);
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    std::string t(text);
    char sep1 = 0, sep2 = 0;
    if (std::sscanf(t.c_str(), "%4d%c%2d%c%2d %2d:%2d", &y, &sep1, &mo, &sep2, &d, &h, &mi) == 7 &&
        sep1 == sep2 && (sep1 == '/' || sep1 == '.' || sep1 == '-')) {
    } else if (t.size() == 14 && std::all_of(t.begin(), t.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }) &&
               std::sscanf(t.c_str(), "%4d%2d%2d%2d%2d%2d", &y, &mo, &d, &h, &mi, &s) == 6) {
    } else {
        return std::nullopt;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) return std::nullopt;
    return daysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400 + h * 3600 + mi * 60 + s;
}

// ---- account activation

std::optional<std::string> accountActivationPassword(const Message& m) {
    std::string subj(trim(m.subject()));
    if (!iequals(normalizeAddress(m.from()), "SERVICE") || !iequals(subj, "Your New Winlink Account")) return std::nullopt;
    // Look for a sentence mentioning "password" with a quoted 6-12 character
    // alphanumeric word in it that is not the callsign (same rule as Pat).
    std::string body = m.bodyText();
    std::string to = m.to().empty() ? std::string() : upper(m.to()[0]);
    std::size_t start = 0;
    while (start < body.size()) {
        std::size_t end = body.find_first_of(".!?", start);
        if (end == std::string::npos) end = body.size();
        std::string sentence = body.substr(start, end - start);
        start = end + 1;
        std::string low;
        for (char c : sentence) low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (low.find("password") == std::string::npos) continue;
        for (std::size_t q = 0; q < sentence.size(); ++q) {
            char open = sentence[q];
            if (open != '\'' && open != '"' && open != '`') continue;
            std::size_t k = q + 1;
            while (k < sentence.size() && std::isalnum(static_cast<unsigned char>(sentence[k]))) ++k;
            std::size_t len = k - q - 1;
            if (k < sentence.size() && (sentence[k] == '\'' || sentence[k] == '"' || sentence[k] == '`') && len >= 6 && len <= 12) {
                std::string word = sentence.substr(q + 1, len);
                if (upper(word) != to) return word;
            }
        }
    }
    return std::string();
}

// ---- Message

std::string Message::header(std::string_view name) const {
    for (const auto& [k, v] : headers_)
        if (iequals(k, name)) return v;
    return {};
}

std::vector<std::string> Message::headers(std::string_view name) const {
    std::vector<std::string> out;
    for (const auto& [k, v] : headers_)
        if (iequals(k, name)) out.push_back(v);
    return out;
}

void Message::setHeader(std::string_view name, std::string value) {
    auto it = std::find_if(headers_.begin(), headers_.end(), [&](const auto& kv) { return iequals(kv.first, name); });
    if (it == headers_.end()) {
        headers_.emplace_back(std::string(name), std::move(value));
        return;
    }
    it->second = std::move(value);
    headers_.erase(std::remove_if(std::next(it), headers_.end(), [&](const auto& kv) { return iequals(kv.first, name); }),
                   headers_.end());
}

void Message::addHeader(std::string_view name, std::string value) { headers_.emplace_back(std::string(name), std::move(value)); }

std::string Message::subject() const { return decodeHeaderWords(header("Subject")); }

std::string Message::bodyText() const { return toUtf8Guess(asText(body_), charsetOf(*this)); }

Message Message::compose(const Draft& d, std::int64_t unixSeconds, std::uint64_t salt) {
    Message m;
    std::string from = normalizeAddress(d.from);
    m.addHeader("Mid", generateMid(from, unixSeconds, salt));
    m.addHeader("Date", formatDate(unixSeconds));
    m.addHeader("Type", "Private");
    m.addHeader("From", from);
    for (const auto& a : d.to) m.addHeader("To", normalizeAddress(a));
    for (const auto& a : d.cc) m.addHeader("Cc", normalizeAddress(a));
    m.addHeader("Subject", encodeHeaderWord(d.subject));
    m.addHeader("Mbo", from);
    m.addHeader("Content-Type", "text/plain; charset=ISO-8859-1");
    m.addHeader("Content-Transfer-Encoding", "8bit");
    std::string body = crlf(utf8ToLatin1(d.body));
    m.body_.assign(body.begin(), body.end());
    m.addHeader("Body", std::to_string(m.body_.size()));
    for (const auto& f : d.files) {
        m.addHeader("File", std::to_string(f.data.size()) + " " + encodeHeaderWord(f.name));
        m.files_.push_back(f);
    }
    return m;
}

Bytes Message::serialize() const {
    Bytes out;
    std::string mid = this->mid();
    append(out, "Mid: " + mid + "\r\n");
    for (const auto& [k, v] : headers_) {
        if (iequals(k, "Mid")) continue;
        append(out, k + ": " + v + "\r\n");
    }
    append(out, std::string_view("\r\n"));
    append(out, body_);
    if (!files_.empty()) append(out, std::string_view("\r\n"));
    for (const auto& f : files_) {
        append(out, f.data);
        append(out, std::string_view("\r\n"));
    }
    return out;
}

std::optional<Message> Message::parse(ByteView raw, std::string* error) {
    auto fail = [&](std::string why) -> std::optional<Message> {
        if (error) *error = std::move(why);
        return std::nullopt;
    };
    std::size_t i = 0;
    while (i < raw.size() && (raw[i] == '\r' || raw[i] == '\n' || raw[i] == ' ' || raw[i] == '\t')) ++i;

    Message m;
    // Headers: lines up to the first empty one.  Continuation lines (leading
    // whitespace) are folded into the previous header.
    for (;;) {
        std::size_t eol = i;
        while (eol < raw.size() && raw[eol] != '\n') ++eol;
        if (eol >= raw.size()) return fail("headers never end");
        std::string_view line = asText(raw.subspan(i, eol - i));
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        i = eol + 1;
        if (line.empty()) break;
        if ((line.front() == ' ' || line.front() == '\t') && !m.headers_.empty()) {
            std::string more = cleanValue(line);
            auto& value = m.headers_.back().second;
            if (!more.empty()) value += (value.empty() ? "" : " ") + more;
            continue;
        }
        auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) return fail("malformed header line: " + std::string(line));
        std::string_view name = trim(line.substr(0, colon));
        // Field names are printable ASCII without spaces or colons (RFC 5322).
        bool nameOk = !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
            auto u = static_cast<unsigned char>(c);
            return u > 0x20 && u < 0x7F;
        });
        if (!nameOk) return fail("malformed header name: " + std::string(line));
        // A stray CR or other control character inside a value would break
        // the message when it is written out again.
        m.headers_.emplace_back(std::string(name), cleanValue(line.substr(colon + 1)));
    }
    if (m.mid().empty()) return fail("no Mid header");

    auto section = [&](std::size_t n, Bytes& dst) -> bool {
        if (raw.size() - i < n) return false;
        dst.assign(raw.begin() + static_cast<std::ptrdiff_t>(i), raw.begin() + static_cast<std::ptrdiff_t>(i + n));
        i += n;
        // Each section ends in CRLF, except that the last may simply end.
        if (i + 1 < raw.size() && raw[i] == '\r' && raw[i + 1] == '\n') i += 2;
        return true;
    };

    std::string bodyLen = m.header("Body");
    std::size_t n = 0;
    try {
        n = bodyLen.empty() ? 0 : static_cast<std::size_t>(std::stoul(bodyLen));
    } catch (...) {
        return fail("Body header is not a number");
    }
    if (!section(n, m.body_)) return fail("body is shorter than its Body header");

    for (const auto& value : m.headers("File")) {
        auto sp = value.find(' ');
        if (sp == std::string::npos) return fail("malformed File header: " + value);
        Attachment a;
        std::size_t size = 0;
        try {
            size = static_cast<std::size_t>(std::stoul(value.substr(0, sp)));
        } catch (...) {
            return fail("File header size is not a number: " + value);
        }
        a.name = decodeHeaderWords(value.substr(sp + 1));
        if (!section(size, a.data)) return fail("attachment " + a.name + " is truncated");
        m.files_.push_back(std::move(a));
    }
    return m;
}

std::string Message::validate() const {
    std::string mid = this->mid();
    if (mid.empty()) return "no message ID";
    if (mid.size() > 12) return "message ID longer than 12 characters";
    if (to().empty() && cc().empty()) return "no recipient";
    if (from().empty()) return "no sender";
    if (body_.empty()) return "empty body";
    std::string subj = header("Subject");
    if (subj.empty()) return "empty subject";
    if (subj.size() > 128) return "subject longer than 128 characters";
    for (const auto& f : files_)
        if (f.name.size() > 255) return "attachment name longer than 255 characters";
    return {};
}

}  // namespace ogm
