#include "system/security/system_permission_profile.hpp"

#include <algorithm>
#include <array>
#include <span>

namespace lcl::security {
namespace {

struct StaticProfile {
    std::string_view appId;
    std::span<const std::string_view> grants;
};

constexpr std::array<std::string_view, 0> kNoGrants{};
constexpr std::array<std::string_view, 2> kVulkanGearsGrants{{
    "graphics.gpu", "graphics.render-node"}};
constexpr std::array<StaticProfile, 3> kProfiles{{
    {"org.lcl.settings", kNoGrants},
    {"org.lcl.sandbox-probe", kNoGrants},
    {"org.lcl.vulkan-gears", kVulkanGearsGrants},
}};

} // namespace

std::vector<std::string> staticSystemImagePermissionGrants(
        std::string_view appId, const std::vector<std::string>& requestedPermissions) {
    const auto profile = std::find_if(kProfiles.begin(), kProfiles.end(),
                                      [appId](const StaticProfile& candidate) {
        return candidate.appId == appId;
    });
    if (profile == kProfiles.end()) return {};

    std::vector<std::string> effective;
    for (const std::string& requested : requestedPermissions) {
        if (std::find(profile->grants.begin(), profile->grants.end(), requested) !=
            profile->grants.end()) {
            effective.push_back(requested);
        }
    }
    std::sort(effective.begin(), effective.end());
    effective.erase(std::unique(effective.begin(), effective.end()), effective.end());
    return effective;
}

} // namespace lcl::security
