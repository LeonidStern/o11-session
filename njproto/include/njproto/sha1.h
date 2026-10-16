// njproto - SHA-1 (RFC 3174) and the NINJAM password hash.
#ifndef NJPROTO_SHA1_H
#define NJPROTO_SHA1_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace njproto {

using Digest = std::array<std::uint8_t, 20>;

class Sha1 {
public:
    Sha1();
    void reset();
    void update(const std::uint8_t* data, std::size_t len);
    void update(const std::string& s);
    Digest finish();                       // the object must be reset() before reuse

private:
    void processBlock(const std::uint8_t block[64]);
    std::uint32_t h_[5];
    std::uint8_t buf_[64];
    std::size_t buf_len_;
    std::uint64_t total_len_;
};

Digest sha1(const std::uint8_t* data, std::size_t len);
Digest sha1(const std::string& s);
std::string digestToHex(const Digest& d);

// passhash = SHA1( SHA1(username + ":" + password) + challenge )  (section 4.2)
Digest passwordHash(const std::string& username, const std::string& password,
                    const std::array<std::uint8_t, 8>& challenge);

// The inner hash SHA1(username + ":" + password), which a server may store
// instead of the clear-text password.
Digest userPassHash(const std::string& username, const std::string& password);
Digest passwordHashFromInner(const Digest& inner, const std::array<std::uint8_t, 8>& challenge);

}  // namespace njproto

#endif  // NJPROTO_SHA1_H
