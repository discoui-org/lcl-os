#include "system/security/application_peer_authenticator.hpp"

#include <array>
#include <utility>

namespace lcl::security {

ApplicationPeerAuthenticator::ApplicationPeerAuthenticator(
        ApplicationPeerAuthenticatorConfig config)
    : config_(std::move(config)) {}

bool ApplicationPeerAuthenticator::isTrustedSessionAppId(
        std::string_view appId) noexcept {
    constexpr std::array<std::string_view, 6> trustedIds{
        "org.lcl.desktop-shell",
        "org.lcl.desktop-wm",
        "org.lcl.mobile-shell",
        "org.lcl.mobile-wm",
        kTrustedUserShellAppId,
        kSystemSettingsAppId,
    };
    for (const auto trustedId : trustedIds) {
        if (appId == trustedId) return true;
    }
    return false;
}

bool ApplicationPeerAuthenticator::authenticate(
        std::string_view appId, uid_t peerUid, gid_t peerGid,
        std::string& error) const {
    error.clear();
    const std::string canonicalAppId(appId);
    if (!AppIdentityRegistry::isValidAppId(canonicalAppId)) {
        error = "peer supplied an invalid canonical app ID";
        return false;
    }

    const bool hasSessionUid = peerUid == config_.sessionUid;
    const bool hasSessionGid = peerGid == config_.sessionGid;
    if (hasSessionUid || hasSessionGid) {
        if (!hasSessionUid || !hasSessionGid ||
            !isTrustedSessionAppId(canonicalAppId)) {
            error = "session peer is not authorized for the claimed app ID";
            return false;
        }
        return true;
    }

    if (peerUid == 0 || peerGid == 0 || peerUid != peerGid ||
        peerUid < config_.identities.firstAppUid ||
        peerUid > config_.identities.lastAppUid) {
        error = "peer credentials are outside the application identity range";
        return false;
    }

    AppIdentityRegistry identities(config_.identities);
    if (!identities.load(error)) return false;

    const auto identity = identities.find(canonicalAppId);
    if (!identity || identity->uid != peerUid || identity->gid != peerGid) {
        error = "peer credentials do not match the claimed app ID";
        return false;
    }
    return true;
}

} // namespace lcl::security
