#pragma once

#include "system/security/bundle_launch_gate.hpp"

#include <string>
#include <sys/types.h>
#include <vector>

namespace lcl::security {

/** Root-owned record that a trusted Settings broker may present to one user. */
struct PendingBundleApproval {
    uid_t userUid{0};
    std::string appId;
    Sha256Digest bundleRecordDigest{};
    BundlePublisherState publisherState{BundlePublisherState::Unverified};
    BundleSourceScope sourceScope{BundleSourceScope::Direct};
    std::string bundlePath;
    std::string displayName;
    std::string appVersion;
};

struct PendingBundleApprovalStoreConfig {
    std::string storePath{"/var/lib/lcl-security/pending-bundle-approvals.v1"};
    uid_t ownerUid{0};
    gid_t ownerGid{0};
};

/**
 * Durable, exact-record queue for unverified direct bundles.
 *
 * sessiond may add a request only after descriptor-based parsing and record
 * creation.  A future trusted Settings/admin broker is the only component
 * that may pair it with BundleApprovalStore::approve or remove it.  This
 * queue itself grants neither launch permission nor runtime capability.
 */
class PendingBundleApprovalStore final {
public:
    explicit PendingBundleApprovalStore(PendingBundleApprovalStoreConfig config = {});

    PendingBundleApprovalStore(const PendingBundleApprovalStore&) = delete;
    PendingBundleApprovalStore& operator=(const PendingBundleApprovalStore&) = delete;

    bool record(uid_t userUid, const BundleRecord& record,
                BundlePublisherState publisherState, BundleSourceScope sourceScope,
                std::string bundlePath, std::string displayName, std::string& error);
    bool remove(uid_t userUid, const BundleRecord& record,
                BundlePublisherState publisherState, std::string& error);
    /** Loads only one user's pending records; an absent store is an empty list. */
    std::vector<PendingBundleApproval> pendingFor(uid_t userUid, std::string& error);

private:
    bool validateConfig(std::string& error) const;
    bool validateStoreParent(std::string& error) const;
    bool loadUnlocked(std::string& error);
    bool persistUnlocked(std::string& error) const;

    PendingBundleApprovalStoreConfig config_;
    std::vector<PendingBundleApproval> pending_;
};

} // namespace lcl::security
