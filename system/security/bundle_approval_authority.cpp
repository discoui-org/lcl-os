#include "system/security/bundle_approval_authority.hpp"

#include <algorithm>
#include <utility>

namespace lcl::security {

BundleApprovalAuthority::BundleApprovalAuthority(BundleApprovalAuthorityConfig config)
    : approvals_(std::move(config.approvals)), pending_(std::move(config.pending)) {}

std::vector<PendingBundleApproval> BundleApprovalAuthority::pendingFor(
    uid_t userUid, std::string& error) {
    return pending_.pendingFor(userUid, error);
}

bool BundleApprovalAuthority::approvePending(uid_t userUid, const BundleRecord& record,
                                              std::string& error) {
    error.clear();
    if (userUid == 0 || !validateBundleRecord(record, error)) {
        if (error.empty()) {
            error = "bundle approval requires a non-root user";
        }
        return false;
    }
    const std::vector<PendingBundleApproval> pending = pending_.pendingFor(userUid, error);
    if (!error.empty()) {
        return false;
    }
    const bool requested = std::any_of(
        pending.begin(), pending.end(), [&record](const PendingBundleApproval& request) {
            return request.appId == record.appId && request.bundleRecordDigest == record.digest &&
                   request.publisherState == BundlePublisherState::Unverified;
        });
    if (!requested) {
        error = "bundle approval refused because this exact bundle is not pending";
        return false;
    }
    if (!approvals_.approve(userUid, record, BundlePublisherState::Unverified, error)) {
        return false;
    }
    if (!pending_.remove(userUid, record, BundlePublisherState::Unverified, error)) {
        // The durable allow record is intentionally not rolled back: a retry
        // is idempotent and repairs the stale presentation record without
        // ever widening the exact-record approval.
        error = "bundle approval was saved but its pending presentation record could not be removed: " +
                error;
        return false;
    }
    return true;
}

bool BundleApprovalAuthority::revoke(uid_t userUid, const BundleRecord& record,
                                      std::string& error) {
    return approvals_.revoke(userUid, record, BundlePublisherState::Unverified, error);
}

} // namespace lcl::security
