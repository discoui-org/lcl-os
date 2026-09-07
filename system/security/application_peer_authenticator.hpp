#pragma once

#include <string>
#include <string_view>
#include <sys/types.h>

#include "system/security/app_identity_registry.hpp"
#include "system/security/session_user.hpp"

namespace lcl::security {

struct ApplicationPeerAuthenticatorConfig {
    AppIdentityRegistryConfig identities;
    uid_t sessionUid{kSessionUserUid};
    gid_t sessionGid{kSessionUserGid};
};

/**
 * Binds a compositor client's claimed canonical app ID to SO_PEERCRED.
 *
 * Sandbox identities are reloaded from the root-owned registry for each
 * surface creation so applications registered after compositor startup are
 * recognized without trusting client-provided launch metadata.
 */
class ApplicationPeerAuthenticator final {
public:
    explicit ApplicationPeerAuthenticator(ApplicationPeerAuthenticatorConfig config);

    bool authenticate(std::string_view appId, uid_t peerUid, gid_t peerGid,
                      std::string& error) const;

private:
    static bool isTrustedSessionAppId(std::string_view appId) noexcept;

    ApplicationPeerAuthenticatorConfig config_;
};

} // namespace lcl::security
