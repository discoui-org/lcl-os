#include "system/security/bundle_launch_gate.hpp"

namespace lcl::security {

BundleLaunchAssessment BundleLaunchGate::assess(uid_t desktopUser, BundleSourceScope sourceScope,
                                                 const BundleRecord& record,
                                                 BundlePublisherState publisherState) const {
    std::string error;
    if (!validateBundleRecord(record, error)) {
        return {BundleLaunchDecision::Reject, "bundle record is invalid: " + error};
    }
    if (desktopUser == 0) {
        return {BundleLaunchDecision::Reject, "session user must not be root"};
    }

    if (sourceScope == BundleSourceScope::System) {
        if (publisherState == BundlePublisherState::SystemImageTrusted) {
            return {BundleLaunchDecision::AllowSystemImage, "trusted by the system image chain"};
        }
        return {BundleLaunchDecision::Reject,
                "a system-scope bundle requires system-image trust"};
    }
    if (publisherState == BundlePublisherState::SignatureVerified) {
        return {BundleLaunchDecision::AllowSignature, "publisher signature verified"};
    }
    if (publisherState != BundlePublisherState::Unverified) {
        return {BundleLaunchDecision::Reject,
                "non-system bundle has an invalid publisher trust state"};
    }

    if (approvals_.isApproved(desktopUser, record, publisherState, error)) {
        return {BundleLaunchDecision::AllowUserApproval,
                "user approved this exact unverified bundle record"};
    }
    if (!error.empty()) {
        return {BundleLaunchDecision::Reject, "could not read bundle approval: " + error};
    }
    return {BundleLaunchDecision::NeedsUserApproval,
            "LCL OS bu uygulamanın yayıncısını doğrulayamadı"};
}

} // namespace lcl::security
