#pragma once

#include "system/security/bundle_approval_store.hpp"

#include <string>
#include <sys/types.h>

namespace lcl::security {

/** Where a directly runnable .app was discovered; this is not an installer scope. */
enum class BundleSourceScope : unsigned char {
    System,
    Machine,
    User,
    Direct,
};

enum class BundleLaunchDecision : unsigned char {
    AllowSystemImage,
    AllowSignature,
    AllowUserApproval,
    NeedsUserApproval,
    Reject,
};

struct BundleLaunchAssessment {
    BundleLaunchDecision decision{BundleLaunchDecision::Reject};
    std::string reason;

    bool allowed() const noexcept {
        return decision == BundleLaunchDecision::AllowSystemImage ||
               decision == BundleLaunchDecision::AllowSignature ||
               decision == BundleLaunchDecision::AllowUserApproval;
    }
};

/**
 * Fail-closed direct-bundle launch policy.
 *
 * A future Ed25519 verifier supplies publisherState. This gate deliberately
 * never derives trust from a writable bundle location and never upgrades an
 * approval into a runtime permission or elevation grant.
 */
class BundleLaunchGate final {
public:
    explicit BundleLaunchGate(BundleApprovalStore& approvals) : approvals_(approvals) {}

    BundleLaunchAssessment assess(uid_t desktopUser, BundleSourceScope sourceScope,
                                  const BundleRecord& record,
                                  BundlePublisherState publisherState) const;

private:
    BundleApprovalStore& approvals_;
};

} // namespace lcl::security
