#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lcl::security {

using Sha256Digest = std::array<std::uint8_t, 32>;

/** Platform-independent SHA-256 used for signed security records. */
Sha256Digest sha256(std::string_view input);

/**
 * Hashes a pinned regular-file descriptor without trusting its pathname.
 * The descriptor is read with pread(), so its current file offset is left
 * untouched. maxBytes is a fail-closed resource limit for untrusted bundles.
 */
std::optional<Sha256Digest> sha256FileDescriptor(int descriptor, std::uint64_t maxBytes);

bool isZeroDigest(const Sha256Digest& digest) noexcept;
std::string hexEncodeDigest(const Sha256Digest& digest);

} // namespace lcl::security
