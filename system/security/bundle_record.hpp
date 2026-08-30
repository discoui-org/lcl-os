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

/** Result of bundle provenance verification, independent of its source path. */
enum class BundlePublisherState : unsigned char {
    Unverified = 1,
    SignatureVerified = 2,
    SystemImageTrusted = 3,
};

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
    // The signed payload excludes the detached Signature.ed25519 envelope.
    Sha256Digest payloadDigest{};
    // Zero means no signature envelope was present. This is included in the
    // outer record digest so an approval cannot survive an envelope change.
    Sha256Digest signatureEnvelopeDigest{};
    Sha256Digest digest{};
};

/** Recreates the bundle record from parser-held descriptors; fails on any mutation or unsafe entry. */
std::optional<BundleRecord> makeBundleRecord(const lcl::core::AppBundleMetadata& metadata,
                                             std::string& error);

/** Validates canonical order, fields and self-consistent digest. */
bool validateBundleRecord(const BundleRecord& record, std::string& error);

/** Returns the versioned, binary payload an Ed25519 signer verifies. */
std::string serializeBundleRecordPayload(const BundleRecord& record);

/** Returns the digest of the canonical payload, for record identity checks. */
Sha256Digest digestBundlePayload(const BundleRecord& record);

/** Returns the outer record digest used by Settings approvals and launch IPC. */
Sha256Digest digestBundleRecord(const BundleRecord& record);

} // namespace lcl::security
