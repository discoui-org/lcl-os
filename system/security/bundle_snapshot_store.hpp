#pragma once

#include <string>
#include <sys/types.h>

#include "system/security/bundle_record.hpp"

namespace lcl::core {
struct AppBundleMetadata;
}

namespace lcl::security {

/**
 * Root-owned immutable copies of verified external bundles.
 *
 * A bundle in /Applications or ~/Applications is deliberately not used as a
 * sandbox mount source: its owner could otherwise alter a previously-approved
 * inode after the approval check.  This store copies the exact BundleRecord
 * payload into a root-controlled snapshot named by its full record digest.
 */
struct BundleSnapshotStoreConfig {
    std::string snapshotsRoot{"/var/lib/lcl-security/bundles"};
    uid_t ownerUid{0};
    gid_t ownerGid{0};
};

class BundleSnapshotStore final {
public:
    explicit BundleSnapshotStore(BundleSnapshotStoreConfig config = {});

    BundleSnapshotStore(const BundleSnapshotStore&) = delete;
    BundleSnapshotStore& operator=(const BundleSnapshotStore&) = delete;

    /**
     * Copies a descriptor-verified user/machine bundle into the protected
     * store, or returns the existing snapshot for the same exact record.
     */
    bool stage(const lcl::core::AppBundleMetadata& source,
               const BundleRecord& record,
               lcl::core::AppBundleMetadata& snapshot,
               std::string& error) const;

private:
    BundleSnapshotStoreConfig config_;
};

} // namespace lcl::security
