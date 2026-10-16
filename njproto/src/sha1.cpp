// njproto - SHA-1 as described in RFC 3174, written from the RFC's algorithm.
#include "njproto/sha1.h"

#include <cstring>

namespace njproto {

static inline std::uint32_t rol(std::uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

Sha1::Sha1() { reset(); }

void Sha1::reset() {
    h_[0] = 0x67452301u;
    h_[1] = 0xEFCDAB89u;
    h_[2] = 0x98BADCFEu;
    h_[3] = 0x10325476u;
    h_[4] = 0xC3D2E1F0u;
    buf_len_ = 0;
    total_len_ = 0;
}

void Sha1::processBlock(const std::uint8_t block[64]) {
    std::uint32_t w[80];
    for (int t = 0; t < 16; ++t)
        w[t] = (static_cast<std::uint32_t>(block[t * 4]) << 24) |
               (static_cast<std::uint32_t>(block[t * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[t * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(block[t * 4 + 3]);
    for (int t = 16; t < 80; ++t) w[t] = rol(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);

    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int t = 0; t < 80; ++t) {
        std::uint32_t f, k;
        if (t < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (t < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (t < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        std::uint32_t temp = rol(a, 5) + f + e + k + w[t];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = temp;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
}

void Sha1::update(const std::uint8_t* data, std::size_t len) {
    total_len_ += len;
    while (len > 0) {
        std::size_t take = 64 - buf_len_;
        if (take > len) take = len;
        std::memcpy(buf_ + buf_len_, data, take);
        buf_len_ += take;
        data += take;
        len -= take;
        if (buf_len_ == 64) {
            processBlock(buf_);
            buf_len_ = 0;
        }
    }
}

void Sha1::update(const std::string& s) {
    update(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

Digest Sha1::finish() {
    // Padding: 0x80, zeros, then the message length in bits as 64-bit big-endian.
    std::uint64_t bits = total_len_ * 8;
    std::uint8_t pad = 0x80;
    update(&pad, 1);
    std::uint8_t zero = 0;
    while (buf_len_ != 56) update(&zero, 1);
    std::uint8_t lenbytes[8];
    for (int i = 0; i < 8; ++i) lenbytes[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    update(lenbytes, 8);

    Digest d;
    for (int i = 0; i < 5; ++i) {
        d[static_cast<std::size_t>(i * 4)] = static_cast<std::uint8_t>(h_[i] >> 24);
        d[static_cast<std::size_t>(i * 4 + 1)] = static_cast<std::uint8_t>(h_[i] >> 16);
        d[static_cast<std::size_t>(i * 4 + 2)] = static_cast<std::uint8_t>(h_[i] >> 8);
        d[static_cast<std::size_t>(i * 4 + 3)] = static_cast<std::uint8_t>(h_[i]);
    }
    return d;
}

Digest sha1(const std::uint8_t* data, std::size_t len) {
    Sha1 s;
    s.update(data, len);
    return s.finish();
}

Digest sha1(const std::string& str) {
    return sha1(reinterpret_cast<const std::uint8_t*>(str.data()), str.size());
}

std::string digestToHex(const Digest& d) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (auto b : d) {
        s.push_back(hex[b >> 4]);
        s.push_back(hex[b & 15]);
    }
    return s;
}

Digest userPassHash(const std::string& username, const std::string& password) {
    Sha1 s;
    s.update(username);
    s.update(":");
    s.update(password);
    return s.finish();
}

Digest passwordHashFromInner(const Digest& inner, const std::array<std::uint8_t, 8>& challenge) {
    Sha1 s;
    s.update(inner.data(), inner.size());
    s.update(challenge.data(), challenge.size());
    return s.finish();
}

Digest passwordHash(const std::string& username, const std::string& password,
                    const std::array<std::uint8_t, 8>& challenge) {
    return passwordHashFromInner(userPassHash(username, password), challenge);
}

}  // namespace njproto
