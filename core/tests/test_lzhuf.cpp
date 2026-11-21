#include <fstream>
#include <iterator>
#include <string>

#include "test.hpp"
#include "ogm/lzhuf.hpp"
#include "ogm/md5.hpp"
#include "ogm/secure_login.hpp"

using ogm::Bytes;

namespace {

Bytes readFile(const std::string& name) {
    std::ifstream in(std::string(OGM_FIXTURE_DIR) + "/lzhuf/" + name, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), {});
}

std::string hex(const std::array<std::uint8_t, 16>& d) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (auto b : d) {
        s += h[b >> 4];
        s += h[b & 15];
    }
    return s;
}

}  // namespace

TEST(md5_rfc1321_vectors) {
    CHECK_EQ(hex(ogm::md5(ogm::asBytes(""))), std::string("d41d8cd98f00b204e9800998ecf8427e"));
    CHECK_EQ(hex(ogm::md5(ogm::asBytes("abc"))), std::string("900150983cd24fb0d6963f7d28e17f72"));
    CHECK_EQ(hex(ogm::md5(ogm::asBytes("message digest"))), std::string("f96b697d7cb7938d525a2f31aaf161d0"));
    CHECK_EQ(hex(ogm::md5(ogm::asBytes(
                 "12345678901234567890123456789012345678901234567890123456789012345678901234567890"))),
             std::string("57edf4a22be3c955ac49da2e2107b67a"));
}

// Vectors from Pat's wl2k-go secure_test.go.
TEST(secure_login_matches_reference) {
    CHECK_EQ(ogm::secureLoginResponse("23753528", "FOOBAR"), std::string("72768415"));
    CHECK_EQ(ogm::secureLoginResponse("23753528", "FooBar"), std::string("95074758"));
}

TEST(crc16_xmodem_check_value) {
    CHECK_EQ(ogm::lzhuf::crc16(ogm::asBytes("123456789")), 0x31C3);
}

TEST(lzhuf_decodes_reference_files) {
    for (const char* name : {"gettysburg.txt", "LPE5NXDVLVSQ.b2f", "pi.txt"}) {
        Bytes plain = readFile(name);
        Bytes packed = readFile(std::string(name) + ".lzh");
        REQUIRE(!plain.empty() && !packed.empty());
        std::string err;
        auto out = ogm::lzhuf::decompress(packed, true, &err);
        if (!out) {
            ::t::fail(__FILE__, __LINE__, std::string(name) + ": " + err);
            continue;
        }
        CHECK(*out == plain);
    }
}

// Byte-for-byte the same output as the reference encoder, so any decoder
// that reads Pat's or JNOS's output reads ours.
TEST(lzhuf_encodes_like_reference) {
    for (const char* name : {"gettysburg.txt", "LPE5NXDVLVSQ.b2f", "pi.txt"}) {
        Bytes plain = readFile(name);
        Bytes packed = readFile(std::string(name) + ".lzh");
        Bytes ours = ogm::lzhuf::compress(plain);
        if (ours != packed)
            ::t::fail(__FILE__, __LINE__, std::string(name) + ": " + std::to_string(ours.size()) + " bytes vs " +
                                              std::to_string(packed.size()));
    }
}

TEST(lzhuf_roundtrips_edge_cases) {
    std::vector<Bytes> cases = {
        Bytes{},
        Bytes{'a'},
        Bytes(5000, 'x'),
        Bytes(70, ' '),
    };
    Bytes noise;
    std::uint32_t x = 12345;
    for (int i = 0; i < 20000; ++i) {
        x = x * 1103515245u + 12345u;
        noise.push_back(static_cast<std::uint8_t>(x >> 16));
    }
    cases.push_back(noise);
    for (const auto& c : cases) {
        Bytes packed = ogm::lzhuf::compress(c);
        auto back = ogm::lzhuf::decompress(packed);
        REQUIRE(back);
        CHECK(*back == c);
        auto plain = ogm::lzhuf::compress(c, false);
        auto back2 = ogm::lzhuf::decompress(plain, false);
        REQUIRE(back2);
        CHECK(*back2 == c);
    }
    CHECK_EQ(ogm::lzhuf::compress(Bytes{}).size(), 6u);
}

TEST(lzhuf_rejects_damage) {
    Bytes packed = ogm::lzhuf::compress(ogm::asBytes(std::string(300, 'q') + "hello"));
    Bytes flipped = packed;
    flipped[10] ^= 0x01;
    std::string err;
    CHECK(!ogm::lzhuf::decompress(flipped, true, &err));
    CHECK_EQ(err, std::string("checksum mismatch"));
    Bytes cut(packed.begin(), packed.begin() + 4);
    CHECK(!ogm::lzhuf::decompress(cut));
    // Without the CRC, a truncated stream is caught by running out of bits.
    Bytes noCrc = ogm::lzhuf::compress(ogm::asBytes(std::string(4000, 'z')), false);
    noCrc.resize(noCrc.size() / 2);
    noCrc[0] = 0xFF;  // claim a longer output than the bits can hold
    noCrc[1] = 0xFF;
    CHECK(!ogm::lzhuf::decompress(noCrc, false));
}
