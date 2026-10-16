// SHA-1 against the RFC 3174 test vectors, plus the password hash.
#include <string>

#include "check.h"
#include "njproto/sha1.h"

using namespace njproto;

TEST(rfc3174_abc) {
    CHECK_EQ(digestToHex(sha1("abc")), "a9993e364706816aba3e25717850c26c9cd0d89d");
}

TEST(rfc3174_two_block) {
    CHECK_EQ(digestToHex(sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
             "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

TEST(rfc3174_million_a) {
    Sha1 s;
    std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) s.update(chunk);
    CHECK_EQ(digestToHex(s.finish()), "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST(rfc3174_repeated) {
    // "0123456701234567..." repeated 10 times (RFC 3174 test 4).
    Sha1 s;
    for (int i = 0; i < 10; ++i) s.update("0123456701234567012345670123456701234567012345670123456701234567");
    CHECK_EQ(digestToHex(s.finish()), "dea356a2cddd90c7a7ecedc5ebb563934f460452");
}

TEST(empty_and_lengths) {
    CHECK_EQ(digestToHex(sha1("")), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    // Boundary lengths around the padding: 55, 56, 63, 64, 65 bytes must agree
    // between one-shot and byte-at-a-time hashing.
    for (std::size_t n : {55u, 56u, 63u, 64u, 65u, 119u, 120u, 128u}) {
        std::string m(n, 'x');
        Sha1 s;
        for (char c : m) s.update(reinterpret_cast<const std::uint8_t*>(&c), 1);
        CHECK_EQ(digestToHex(s.finish()), digestToHex(sha1(m)));
    }
}

TEST(password_hash) {
    // passhash = SHA1( SHA1("user:pass") + challenge ), built two ways.
    std::array<std::uint8_t, 8> ch = {1, 2, 3, 4, 5, 6, 7, 8};
    Digest inner = sha1("alice:secret");
    CHECK_EQ(inner, userPassHash("alice", "secret"));
    std::string outer(inner.begin(), inner.end());
    outer.append(ch.begin(), ch.end());
    CHECK_EQ(passwordHash("alice", "secret", ch), sha1(outer));
    CHECK_EQ(passwordHashFromInner(inner, ch), sha1(outer));
    // Different challenge or password changes the result; an empty password is fine.
    std::array<std::uint8_t, 8> ch2 = {9, 2, 3, 4, 5, 6, 7, 8};
    CHECK(passwordHash("alice", "secret", ch2) != passwordHash("alice", "secret", ch));
    CHECK(passwordHash("alice", "", ch) != passwordHash("alice", "secret", ch));
    Digest anon_inner = sha1("anonymous:x:");      // username + ":" + empty password
    CHECK_EQ(passwordHash("anonymous:x", "", ch),
             sha1(std::string(anon_inner.begin(), anon_inner.end()) + std::string(ch.begin(), ch.end())));
}

TEST_MAIN()
