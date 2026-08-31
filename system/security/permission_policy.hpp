#pragma once

#include <string_view>

namespace lcl::security {

/** Where an already-granted permission is enforced. */
enum class PermissionEvaluationPhase : unsigned char {
    /** Changes the sandbox shape, so it takes effect only at the next launch. */
    LaunchTime,
    /** A trusted portal checks it for each individual user-visible operation. */
    BrokerTime,
};

/**
 * Closed vocabulary for manifest and PermissionStore decision names.
 *
 * `admin.elevation` only permits an app to ask the trusted elevation broker;
 * it never grants administrator access and always requires fresh visible
 * consent.  Files, clipboard, media, notifications and launch-like access
 * are deliberately broker-time because their concrete object/scope is chosen
 * by a trusted system surface, not by the app's filesystem namespace.
 */
struct PermissionDefinition {
    std::string_view identifier;
    PermissionEvaluationPhase evaluationPhase;
    bool requiresProcessRestart;
    bool requiresVisibleConsent;
};

/** Returns null for an unknown permission; unknown requests are invalid. */
const PermissionDefinition* findPermissionDefinition(std::string_view identifier) noexcept;

inline bool isKnownPermission(std::string_view identifier) noexcept {
    return findPermissionDefinition(identifier) != nullptr;
}

} // namespace lcl::security
