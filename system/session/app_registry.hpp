#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "system/session/app_bundle_parser.hpp"

namespace lcl::session {

/** Session-owned, cached application catalog. */
class AppRegistry {
public:
    explicit AppRegistry(std::vector<std::string> searchPaths = {
        "/home/user/Applications", "/Applications"});

    void refresh();
    const std::vector<core::AppBundleMetadata>& entries() const noexcept { return m_entries; }
    std::optional<core::AppBundleMetadata> find(const std::string& target) const;

private:
    std::vector<std::string> m_searchPaths;
    std::vector<core::AppBundleMetadata> m_entries;
    std::unordered_map<std::string, size_t> m_byAppId;
};

} // namespace lcl::session
