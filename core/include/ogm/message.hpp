// The Winlink message structure (B2F): RFC 822-style headers, then the body
// and attachments, each preceded by its exact byte count in the headers.
//
//     Mid: 1AB2C3D4E5F6
//     Date: 2026/11/18 23:05
//     Type: Private
//     From: N0CALL
//     To: SMTP:friend@example.com
//     Subject: Hello
//     Mbo: N0CALL
//     Body: 13
//     File: 2048 photo.jpg
//     <CRLF>
//     body bytes <CRLF> file bytes <CRLF>
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ogm/common.hpp"

namespace ogm {

struct Attachment {
    std::string name;  // UTF-8
    Bytes data;
};

class Message {
public:
    // Builds a new outgoing message.  Text is UTF-8; the body is stored as
    // ISO-8859-1 (what Winlink Express and the CMS expect), with typographic
    // quotes and dashes turned into plain ones and anything else that does not
    // fit replaced by '?'.  `unixSeconds` is wall-clock UTC from the shell;
    // `salt` is anything that differs between messages composed in the same
    // second (a counter is fine).
    struct Draft {
        std::string from;                 // callsign
        std::vector<std::string> to, cc;  // "N0CALL", "N0CALL@winlink.org" or "a@b.c"
        std::string subject;
        std::string body;
        std::vector<Attachment> files;
    };
    static Message compose(const Draft& d, std::int64_t unixSeconds, std::uint64_t salt = 0);

    // Parses a received message; nullopt (and a reason) if it is malformed.
    static std::optional<Message> parse(ByteView raw, std::string* error = nullptr);

    // The wire form: Mid first, then the other headers in order, then the
    // body and files.
    Bytes serialize() const;

    // Headers (names compared case-insensitively; first match wins for get).
    std::string header(std::string_view name) const;
    std::vector<std::string> headers(std::string_view name) const;
    void setHeader(std::string_view name, std::string value);  // replace all, or append
    void addHeader(std::string_view name, std::string value);
    const std::vector<std::pair<std::string, std::string>>& allHeaders() const { return headers_; }

    std::string mid() const { return header("Mid"); }
    std::string from() const { return header("From"); }
    std::vector<std::string> to() const { return headers("To"); }
    std::vector<std::string> cc() const { return headers("Cc"); }
    std::string subject() const;      // decoded to UTF-8
    std::string date() const { return header("Date"); }
    std::string bodyText() const;     // decoded to UTF-8, CRLF kept
    const Bytes& rawBody() const { return body_; }
    const std::vector<Attachment>& files() const { return files_; }

    // Problems that would make the CMS refuse the message, or "" if none.
    std::string validate() const;

private:
    std::vector<std::pair<std::string, std::string>> headers_;
    Bytes body_;
    std::vector<Attachment> files_;
};

// The CMS's "Your New Winlink Account" message from SERVICE: returns the
// generated password quoted in it ("" if it is that message but no password
// could be found), or nullopt for any other message.
std::optional<std::string> accountActivationPassword(const Message& m);

// "N0CALL@winlink.org" -> "N0CALL"; "a@b.c" -> "SMTP:a@b.c"; "n0call" -> "N0CALL".
std::string normalizeAddress(std::string_view addr);

// 12-character message ID: base32 of MD5 over the inputs.
std::string generateMid(std::string_view callsign, std::int64_t unixSeconds, std::uint64_t salt);

// "YYYY/MM/DD HH:MM" in UTC, and back (nullopt if unparseable).
std::string formatDate(std::int64_t unixSeconds);
std::optional<std::int64_t> parseDate(std::string_view text);

// Character-set helpers, exposed for tests and the UI.
std::string utf8ToLatin1(std::string_view utf8, bool* changed = nullptr);
std::string latin1ToUtf8(std::string_view latin1);
bool isValidUtf8(std::string_view s);
std::string encodeHeaderWord(std::string_view utf8);  // RFC 2047 Q, only if needed
std::string decodeHeaderWords(std::string_view raw);  // RFC 2047 Q/B, to UTF-8

}  // namespace ogm
