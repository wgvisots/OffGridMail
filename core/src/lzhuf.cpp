// LZHUF, after LZHUF.C (Haruhiko Okumura, Haruyasu Yoshizaki, Kenji Rikitake,
// 1988; "use, distribute, and modify this program freely"), as adapted for the
// FBB B2 protocol in JNOS and in Pat's wl2k-go (MIT).
#include "ogm/lzhuf.hpp"

#include <array>
#include <cstring>
#include <memory>

namespace ogm::lzhuf {
namespace {

constexpr int N = 2048;                       // ring buffer size
constexpr int F = 60;                         // lookahead size
constexpr int THRESHOLD = 2;
constexpr int NIL = N;                        // leaf of the search tree
constexpr int N_CHAR = 256 - THRESHOLD + F;   // literal bytes plus match lengths
constexpr int T = N_CHAR * 2 - 1;             // Huffman table size
constexpr int R = T - 1;                      // root position
constexpr unsigned MAX_FREQ = 0x8000;
constexpr std::uint32_t kMaxSize = 64u * 1024u * 1024u;  // refuse absurd headers

// Upper 6 bits of a match position: code and length for encoding...
constexpr std::uint8_t p_len[64] = {
    3, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8};
constexpr std::uint8_t p_code[64] = {
    0x00, 0x20, 0x30, 0x40, 0x50, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80, 0x88, 0x90, 0x94, 0x98, 0x9C,
    0xA0, 0xA4, 0xA8, 0xAC, 0xB0, 0xB4, 0xB8, 0xBC, 0xC0, 0xC2, 0xC4, 0xC6, 0xC8, 0xCA, 0xCC, 0xCE,
    0xD0, 0xD2, 0xD4, 0xD6, 0xD8, 0xDA, 0xDC, 0xDE, 0xE0, 0xE2, 0xE4, 0xE6, 0xE8, 0xEA, 0xEC, 0xEE,
    0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF};

// ...and the decoding tables, derived from the encoding ones: a byte read
// from the stream that starts with code i decodes to position bits i.
struct DecodeTables {
    std::array<std::uint8_t, 256> code{}, len{};
    constexpr DecodeTables() {
        for (int i = 0; i < 64; ++i) {
            int span = 1 << (8 - p_len[i]);
            for (int k = 0; k < span; ++k) {
                code[static_cast<std::size_t>(p_code[i] + k)] = static_cast<std::uint8_t>(i);
                len[static_cast<std::size_t>(p_code[i] + k)] = p_len[i];
            }
        }
    }
};
constexpr DecodeTables kDecode;

constexpr std::array<std::uint16_t, 256> makeCrcTable() {
    std::array<std::uint16_t, 256> t{};
    for (int i = 0; i < 256; ++i) {
        std::uint16_t c = static_cast<std::uint16_t>(i << 8);
        for (int k = 0; k < 8; ++k) c = static_cast<std::uint16_t>((c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1);
        t[static_cast<std::size_t>(i)] = c;
    }
    return t;
}
constexpr auto kCrcTable = makeCrcTable();

// Adaptive Huffman tree shared by the encoder and decoder.
struct Huffman {
    unsigned freq[T + 1];
    int prnt[T + N_CHAR];
    int son[T];

    Huffman() {
        for (int i = 0; i < N_CHAR; ++i) {
            freq[i] = 1;
            son[i] = i + T;
            prnt[i + T] = i;
        }
        for (int i = 0, j = N_CHAR; j <= R; i += 2, ++j) {
            freq[j] = freq[i] + freq[i + 1];
            son[j] = i;
            prnt[i] = prnt[i + 1] = j;
        }
        freq[T] = 0xffff;
        prnt[R] = 0;
    }

    void reconst() {
        int j = 0;
        for (int i = 0; i < T; ++i) {
            if (son[i] >= T) {
                freq[j] = (freq[i] + 1) / 2;
                son[j] = son[i];
                ++j;
            }
        }
        for (int i = 0, jj = N_CHAR; jj < T; i += 2, ++jj) {
            unsigned f = freq[jj] = freq[i] + freq[i + 1];
            int k = jj - 1;
            while (f < freq[k]) --k;
            ++k;
            std::memmove(&freq[k + 1], &freq[k], static_cast<std::size_t>(jj - k) * sizeof freq[0]);
            freq[k] = f;
            std::memmove(&son[k + 1], &son[k], static_cast<std::size_t>(jj - k) * sizeof son[0]);
            son[k] = i;
        }
        for (int i = 0; i < T; ++i) {
            int k = son[i];
            if (k >= T) {
                prnt[k] = i;
            } else {
                prnt[k] = prnt[k + 1] = i;
            }
        }
    }

    void update(int c) {
        if (freq[R] == MAX_FREQ) reconst();
        c = prnt[c + T];
        do {
            unsigned k = ++freq[c];
            int l = c + 1;
            if (k > freq[l]) {
                while (k > freq[++l]) {
                }
                --l;
                freq[c] = freq[l];
                freq[l] = k;

                int i = son[c];
                prnt[i] = l;
                if (i < T) prnt[i + 1] = l;

                int j = son[l];
                son[l] = i;

                prnt[j] = c;
                if (j < T) prnt[j + 1] = c;
                son[c] = j;

                c = l;
            }
        } while ((c = prnt[c]) != 0);
    }
};

class Encoder {
public:
    explicit Encoder(Bytes& out) : out_(out) {
        for (int i = N + 1; i <= N + 256; ++i) rson_[i] = NIL;
        for (int i = 0; i < N; ++i) dad_[i] = NIL;
    }

    void run(ByteView in) {
        std::size_t pos = 0;
        auto next = [&](int& c) {
            if (pos >= in.size()) return false;
            c = in[pos++];
            return true;
        };
        int s = 0, r = N - F, c = 0, len = 0;
        for (int i = s; i < r; ++i) text_[i] = ' ';
        for (len = 0; len < F && next(c); ++len) text_[r + len] = static_cast<std::uint8_t>(c);
        if (len == 0) return;
        for (int i = 1; i <= F; ++i) insertNode(r - i);
        insertNode(r);
        do {
            if (matchLength_ > len) matchLength_ = len;
            if (matchLength_ <= THRESHOLD) {
                matchLength_ = 1;
                encodeChar(text_[r]);
            } else {
                encodeChar(static_cast<unsigned>(255 - THRESHOLD + matchLength_));
                encodePosition(static_cast<unsigned>(matchPosition_));
            }
            int last = matchLength_;
            int i = 0;
            for (; i < last && next(c); ++i) {
                deleteNode(s);
                text_[s] = static_cast<std::uint8_t>(c);
                if (s < F - 1) text_[s + N] = static_cast<std::uint8_t>(c);
                s = (s + 1) & (N - 1);
                r = (r + 1) & (N - 1);
                insertNode(r);
            }
            while (i++ < last) {
                deleteNode(s);
                s = (s + 1) & (N - 1);
                r = (r + 1) & (N - 1);
                if (--len) insertNode(r);
            }
        } while (len > 0);
        if (putlen_) out_.push_back(static_cast<std::uint8_t>(putbuf_ >> 8));
    }

private:
    void insertNode(int r) {
        int cmp = 1;
        const std::uint8_t* key = &text_[r];
        int p = N + 1 + key[0];
        rson_[r] = lson_[r] = NIL;
        matchLength_ = 0;
        for (;;) {
            if (cmp >= 0) {
                if (rson_[p] != NIL) {
                    p = rson_[p];
                } else {
                    rson_[p] = r;
                    dad_[r] = p;
                    return;
                }
            } else {
                if (lson_[p] != NIL) {
                    p = lson_[p];
                } else {
                    lson_[p] = r;
                    dad_[r] = p;
                    return;
                }
            }
            int i = 1;
            for (; i < F; ++i)
                if ((cmp = key[i] - text_[p + i]) != 0) break;
            if (i > THRESHOLD) {
                if (i > matchLength_) {
                    matchPosition_ = ((r - p) & (N - 1)) - 1;
                    if ((matchLength_ = i) >= F) break;
                }
                if (i == matchLength_) {
                    int c = ((r - p) & (N - 1)) - 1;
                    if (c < matchPosition_) matchPosition_ = c;
                }
            }
        }
        dad_[r] = dad_[p];
        lson_[r] = lson_[p];
        rson_[r] = rson_[p];
        dad_[lson_[p]] = r;
        dad_[rson_[p]] = r;
        if (rson_[dad_[p]] == p) {
            rson_[dad_[p]] = r;
        } else {
            lson_[dad_[p]] = r;
        }
        dad_[p] = NIL;
    }

    void deleteNode(int p) {
        if (dad_[p] == NIL) return;
        int q;
        if (rson_[p] == NIL) {
            q = lson_[p];
        } else if (lson_[p] == NIL) {
            q = rson_[p];
        } else {
            q = lson_[p];
            if (rson_[q] != NIL) {
                do {
                    q = rson_[q];
                } while (rson_[q] != NIL);
                rson_[dad_[q]] = lson_[q];
                dad_[lson_[q]] = dad_[q];
                lson_[q] = lson_[p];
                dad_[lson_[p]] = q;
            }
            rson_[q] = rson_[p];
            dad_[rson_[p]] = q;
        }
        dad_[q] = dad_[p];
        if (rson_[dad_[p]] == p) {
            rson_[dad_[p]] = q;
        } else {
            lson_[dad_[p]] = q;
        }
        dad_[p] = NIL;
    }

    void putCode(int l, unsigned c) {
        putbuf_ |= c >> putlen_;
        if ((putlen_ += l) >= 8) {
            out_.push_back(static_cast<std::uint8_t>(putbuf_ >> 8));
            if ((putlen_ -= 8) >= 8) {
                out_.push_back(static_cast<std::uint8_t>(putbuf_));
                putlen_ -= 8;
                putbuf_ = c << (l - putlen_);
            } else {
                putbuf_ <<= 8;
            }
        }
    }

    void encodeChar(unsigned c) {
        unsigned i = 0;
        int j = 0;
        int k = huff_.prnt[c + T];
        do {
            i >>= 1;
            if (k & 1) i += 0x8000;
            ++j;
        } while ((k = huff_.prnt[k]) != R);
        putCode(j, i);
        huff_.update(static_cast<int>(c));
    }

    void encodePosition(unsigned c) {
        unsigned i = c >> 6;
        putCode(p_len[i], static_cast<unsigned>(p_code[i]) << 8);
        putCode(6, (c & 0x3f) << 10);
    }

    Bytes& out_;
    Huffman huff_;
    std::uint8_t text_[N + F - 1] = {};
    int lson_[N + 1] = {}, rson_[N + 257] = {}, dad_[N + 1] = {};
    int matchPosition_ = 0, matchLength_ = 0;
    unsigned putbuf_ = 0;
    int putlen_ = 0;
};

class Decoder {
public:
    Decoder(ByteView in, std::size_t pos) : in_(in), pos_(pos) {}

    bool run(std::uint32_t size, Bytes& out) {
        for (int i = 0; i < N - F; ++i) text_[i] = ' ';
        int r = N - F;
        out.reserve(size);
        while (out.size() < size) {
            int c = decodeChar();
            if (overrun_) return false;
            if (c < 256) {
                out.push_back(static_cast<std::uint8_t>(c));
                text_[r++] = static_cast<std::uint8_t>(c);
                r &= N - 1;
            } else {
                int i = (r - decodePosition() - 1) & (N - 1);
                int j = c - 255 + THRESHOLD;
                for (int k = 0; k < j && out.size() < size; ++k) {
                    std::uint8_t b = text_[(i + k) & (N - 1)];
                    out.push_back(b);
                    text_[r++] = b;
                    r &= N - 1;
                }
            }
            if (overrun_) return false;
        }
        return true;
    }

private:
    int getBit() {
        if (bits_ == 0) {
            if (pos_ >= in_.size()) {
                // Past the end: the original reads zeros.  Allow a byte of
                // slack for the final partial code, then call it truncated.
                if (++padding_ > 2) overrun_ = true;
                cur_ = 0;
            } else {
                cur_ = in_[pos_++];
            }
            bits_ = 8;
        }
        --bits_;
        return (cur_ >> bits_) & 1;
    }

    int getByte() {
        int v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 1) | getBit();
        return v;
    }

    int decodeChar() {
        int c = huff_.son[R];
        while (c < T) c = huff_.son[c + getBit()];
        c -= T;
        huff_.update(c);
        return c;
    }

    int decodePosition() {
        int i = getByte();
        int c = kDecode.code[static_cast<std::size_t>(i)] << 6;
        int j = kDecode.len[static_cast<std::size_t>(i)] - 2;
        while (j-- > 0) i = (i << 1) + getBit();
        return c | (i & 0x3f);
    }

    ByteView in_;
    std::size_t pos_;
    Huffman huff_;
    std::uint8_t text_[N] = {};
    std::uint8_t cur_ = 0;
    int bits_ = 0;
    int padding_ = 0;
    bool overrun_ = false;
};

void putLe32(Bytes& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

}  // namespace

std::uint16_t crc16(ByteView data) {
    std::uint16_t crc = 0;
    for (std::uint8_t b : data)
        crc = static_cast<std::uint16_t>((crc << 8) ^ kCrcTable[((crc >> 8) ^ b) & 0xff]);
    return crc;
}

Bytes compress(ByteView data, bool b2) {
    Bytes body;
    putLe32(body, static_cast<std::uint32_t>(data.size()));
    {
        auto enc = std::make_unique<Encoder>(body);  // ~40 KB of tables: keep off the stack
        enc->run(data);
    }
    if (!b2) return body;
    std::uint16_t crc = crc16(body);
    Bytes out{static_cast<std::uint8_t>(crc & 0xff), static_cast<std::uint8_t>(crc >> 8)};
    append(out, body);
    return out;
}

std::optional<Bytes> decompress(ByteView data, bool b2, std::string* error) {
    auto fail = [&](const char* why) -> std::optional<Bytes> {
        if (error) *error = why;
        return std::nullopt;
    };
    std::size_t pos = 0;
    if (b2) {
        if (data.size() < 2) return fail("too short for the CRC");
        std::uint16_t want = static_cast<std::uint16_t>(data[0] | data[1] << 8);
        if (crc16(data.subspan(2)) != want) return fail("checksum mismatch");
        pos = 2;
    }
    if (data.size() < pos + 4) return fail("too short for the length");
    std::uint32_t size = 0;
    for (int i = 0; i < 4; ++i) size |= static_cast<std::uint32_t>(data[pos + static_cast<std::size_t>(i)]) << (8 * i);
    pos += 4;
    if (size > kMaxSize) return fail("length header is implausible");
    Bytes out;
    auto dec = std::make_unique<Decoder>(data, pos);
    if (!dec->run(size, out)) return fail("compressed data is truncated");
    return out;
}

}  // namespace ogm::lzhuf
