#include "system/security/sha256.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

namespace lcl::security {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr std::uint32_t rotateRight(std::uint32_t value, std::uint32_t shift) {
    return (value >> shift) | (value << (32 - shift));
}

class Sha256Context final {
public:
    void update(std::string_view input) {
        for (const unsigned char byte : input) {
            buffer_[bufferLength_++] = byte;
            if (bufferLength_ == buffer_.size()) {
                transform();
                bitLength_ += 512;
                bufferLength_ = 0;
            }
        }
    }

    Sha256Digest finish() {
        const std::uint64_t totalBits = bitLength_ + static_cast<std::uint64_t>(bufferLength_) * 8;
        buffer_[bufferLength_++] = 0x80;
        if (bufferLength_ > 56) {
            while (bufferLength_ < 64) {
                buffer_[bufferLength_++] = 0;
            }
            transform();
            bufferLength_ = 0;
        }
        while (bufferLength_ < 56) {
            buffer_[bufferLength_++] = 0;
        }
        for (int byte = 7; byte >= 0; --byte) {
            buffer_[bufferLength_++] = static_cast<std::uint8_t>(totalBits >> (byte * 8));
        }
        transform();

        Sha256Digest digest{};
        for (std::size_t word = 0; word < state_.size(); ++word) {
            digest[word * 4] = static_cast<std::uint8_t>(state_[word] >> 24);
            digest[word * 4 + 1] = static_cast<std::uint8_t>(state_[word] >> 16);
            digest[word * 4 + 2] = static_cast<std::uint8_t>(state_[word] >> 8);
            digest[word * 4 + 3] = static_cast<std::uint8_t>(state_[word]);
        }
        return digest;
    }

private:
    void transform() {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const std::size_t offset = index * 4;
            words[index] = (static_cast<std::uint32_t>(buffer_[offset]) << 24) |
                           (static_cast<std::uint32_t>(buffer_[offset + 1]) << 16) |
                           (static_cast<std::uint32_t>(buffer_[offset + 2]) << 8) |
                           static_cast<std::uint32_t>(buffer_[offset + 3]);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const std::uint32_t sigma0 = rotateRight(words[index - 15], 7) ^
                                         rotateRight(words[index - 15], 18) ^ (words[index - 15] >> 3);
            const std::uint32_t sigma1 = rotateRight(words[index - 2], 17) ^
                                         rotateRight(words[index - 2], 19) ^ (words[index - 2] >> 10);
            words[index] = words[index - 16] + sigma0 + words[index - 7] + sigma1;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t round = 0; round < words.size(); ++round) {
            const std::uint32_t sum1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
            const std::uint32_t choose = (e & f) ^ ((~e) & g);
            const std::uint32_t temporary1 = h + sum1 + choose + kRoundConstants[round] + words[round];
            const std::uint32_t sum0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_ = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t bufferLength_{0};
    std::uint64_t bitLength_{0};
};

} // namespace

Sha256Digest sha256(std::string_view input) {
    Sha256Context context;
    context.update(input);
    return context.finish();
}

std::optional<Sha256Digest> sha256FileDescriptor(int descriptor, std::uint64_t maxBytes) {
    struct stat status {};
    if (descriptor < 0 || fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_nlink != 1 || status.st_size < 0 ||
        static_cast<std::uint64_t>(status.st_size) > maxBytes) {
        return std::nullopt;
    }

    Sha256Context context;
    std::array<char, 64 * 1024> buffer{};
    std::uint64_t offset = 0;
    const std::uint64_t fileSize = static_cast<std::uint64_t>(status.st_size);
    while (offset < fileSize) {
        const std::size_t wanted = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), fileSize - offset));
        const ssize_t count = pread(descriptor, buffer.data(), wanted, static_cast<off_t>(offset));
        if (count > 0) {
            context.update(std::string_view(buffer.data(), static_cast<std::size_t>(count)));
            offset += static_cast<std::uint64_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return std::nullopt;
    }
    struct stat finalStatus {};
    if (fstat(descriptor, &finalStatus) != 0 || !S_ISREG(finalStatus.st_mode) ||
        finalStatus.st_nlink != 1 || finalStatus.st_dev != status.st_dev ||
        finalStatus.st_ino != status.st_ino || finalStatus.st_size != status.st_size) {
        return std::nullopt;
    }
    return context.finish();
}

bool isZeroDigest(const Sha256Digest& digest) noexcept {
    for (const std::uint8_t byte : digest) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

std::string hexEncodeDigest(const Sha256Digest& digest) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const std::uint8_t byte : digest) {
        result.push_back(kHex[byte >> 4]);
        result.push_back(kHex[byte & 0x0F]);
    }
    return result;
}

} // namespace lcl::security
