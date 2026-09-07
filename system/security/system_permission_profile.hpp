#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace lcl::security {

/**
 * Rootfs-owned grants for immutable system bundles.
 *
 * The table is compiled into the canonical userspace image. A manifest may
 * request more capabilities, but only entries present in this policy become
 * effective. An unknown system app receives an empty grant set.
 */
std::vector<std::string> staticSystemImagePermissionGrants(
    std::string_view appId, const std::vector<std::string>& requestedPermissions);

} // namespace lcl::security
