#pragma once

#include "system/security/sha256.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lcl::core {
struct AppBundleMetadata;
}

namespace lcl::security {

inline constexpr std::uint32_t kBundleRecordVersion = 1;

/** A regular file included in the canonical identity of a direct .app bundle. */
struct BundleFileDigest {
    std::string relativePath;
    std::uint16_t mode{0};
    Sha256Digest digest{};
};

/**
 * Canonical, path-independent identity for a schema-checked direct .app bundle.
 *
 * This record is deliberately independent of installation: it is rebuilt from
 * the bundle's held directory descriptor whenever it is catalogued or launched.
 * A root Signature.ed25519 detached envelope is deliberately excluded: its
 * signature covers this record and therefore cannot cover its own bytes.
 */
struct BundleRecord {
    std::uint32_t recordVersion{kBundleRecordVersion};
    std::string appId;
    std::string appVersion;
    std::string type;
    std::string runtime;
    std::vector<std::string> requestedPermissions;
    std::vector<BundleFileDigest> files;
    Sha256Digest digest{};
};

/** Recreates the bundle record from parser-held descriptors; fails on any mutation or unsafe entry. */
std::optional<BundleRecord> makeBundleRecord(const lcl::core::AppBundleMetadata& metadata,
                                             std::string& error);

/** Validates canonical order, fields and self-consistent digest. */
bool validateBundleRecord(const BundleRecord& record, std::string& error);

/** Returns the versioned canonical digest used by signatures and Settings approvals. */
Sha256Digest digestBundleRecord(const BundleRecord& record);

} // namespace lcl::security
