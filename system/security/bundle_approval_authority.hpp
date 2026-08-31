#pragma once

#include "system/security/bundle_approval_store.hpp"
#include "system/security/pending_bundle_approval_store.hpp"

#include <string>
#include <sys/types.h>
#include <vector>

namespace lcl::security {

struct BundleApprovalAuthorityConfig {
    BundleApprovalStoreConfig approvals{
        .storePath = "/var/lib/lcl-security/bundle-approvals.v1",
        .ownerUid = 0,
        .ownerGid = 0,
    };
    PendingBundleApprovalStoreConfig pending{};
};

/**
 * Root-side transaction coordinator for a future trusted Settings/admin IPC.
 *
 * It refuses to approve an arbitrary BundleRecord: the exact unverified
 * record must first be present in sessiond's pending queue for that user.
 * The approval itself remains a normal-sandbox launch approval only.
 */
class BundleApprovalAuthority final {
public:
    explicit BundleApprovalAuthority(BundleApprovalAuthorityConfig config = {});

    BundleApprovalAuthority(const BundleApprovalAuthority&) = delete;
    BundleApprovalAuthority& operator=(const BundleApprovalAuthority&) = delete;

    std::vector<PendingBundleApproval> pendingFor(uid_t userUid, std::string& error);
    /** Authorizes only an identity already present in the pending queue. */
    bool approvePending(uid_t userUid, const std::string& appId,
                        const Sha256Digest& bundleRecordDigest, std::string& error);
    bool approvePending(uid_t userUid, const BundleRecord& record, std::string& error);
    bool revoke(uid_t userUid, const std::string& appId,
                const Sha256Digest& bundleRecordDigest, std::string& error);
    bool revoke(uid_t userUid, const BundleRecord& record, std::string& error);

private:
    BundleApprovalStore approvals_;
    PendingBundleApprovalStore pending_;
};

} // namespace lcl::security
