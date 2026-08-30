#pragma once

#include "system/security/bundle_record.hpp"

#include <cstddef>
#include <string>
#include <sys/types.h>
#include <vector>

namespace lcl::security {

/** The verifier result that a user decision is bound to. */
enum class BundlePublisherState : unsigned char {
    Unverified = 1,
    SignatureVerified = 2,
    SystemImageTrusted = 3,
};

struct BundleApprovalStoreConfig {
    std::string storePath;
    uid_t ownerUid{0};
    gid_t ownerGid{0};
};

struct BundleApproval {
    uid_t userUid{0};
    std::string appId;
    Sha256Digest bundleRecordDigest{};
    BundlePublisherState publisherState{BundlePublisherState::Unverified};
};

/**
 * Root-owned persistent allowlist for direct, unverifiable .app bundles.
 *
 * The Settings service is the only intended writer. An entry grants only a
 * normal sandboxed launch; it never grants a runtime permission or elevation.
 */
class BundleApprovalStore final {
public:
    explicit BundleApprovalStore(BundleApprovalStoreConfig config);

    BundleApprovalStore(const BundleApprovalStore&) = delete;
    BundleApprovalStore& operator=(const BundleApprovalStore&) = delete;

    bool load(std::string& error);
    bool approve(uid_t userUid, const BundleRecord& record, BundlePublisherState publisherState,
                 std::string& error);
    /** Idempotently removes exactly one user's normal-sandbox launch approval. */
    bool revoke(uid_t userUid, const BundleRecord& record, BundlePublisherState publisherState,
                std::string& error);
    bool isApproved(uid_t userUid, const BundleRecord& record, BundlePublisherState publisherState,
                    std::string& error);
    std::size_t size() const noexcept { return approvals_.size(); }

private:
    bool validateConfig(std::string& error) const;
    bool validateStoreParent(std::string& error) const;
    bool loadUnlocked(std::string& error);
    bool persistUnlocked(std::string& error) const;

    BundleApprovalStoreConfig config_;
    std::vector<BundleApproval> approvals_;
};

} // namespace lcl::security
