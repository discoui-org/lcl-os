#include "system/session/app_registry.hpp"

#include <algorithm>
#include <filesystem>

namespace lcl::session {
namespace {

bool targetMatchesBundle(const core::AppBundleMetadata& app, const std::string& target) {
    const std::filesystem::path bundlePath(app.bundlePath);
    if (target == app.bundlePath || target == bundlePath.filename().string()) return true;
    if (target.find('/') == std::string::npos) {
        std::string stem = bundlePath.stem().string();
        return target == stem || target == stem + ".app";
    }
    std::error_code error;
    return std::filesystem::equivalent(target, app.bundlePath, error) && !error;
}

} // namespace

AppRegistry::AppRegistry(std::vector<std::string> searchPaths)
    : m_searchPaths(std::move(searchPaths)) {}

void AppRegistry::refresh() {
    m_entries.clear();
    m_byAppId.clear();
    for (const auto& path : m_searchPaths) {
        for (auto app : core::AppBundleParser::scanDirectory(path)) {
            if (!app.valid || app.appId.empty() || m_byAppId.contains(app.appId)) continue;
            m_byAppId.emplace(app.appId, m_entries.size());
            m_entries.push_back(std::move(app));
        }
    }
    std::sort(m_entries.begin(), m_entries.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.appId < rhs.appId;
    });
    m_byAppId.clear();
    for (size_t i = 0; i < m_entries.size(); ++i) m_byAppId.emplace(m_entries[i].appId, i);
}

std::optional<core::AppBundleMetadata> AppRegistry::find(const std::string& target) const {
    if (const auto found = m_byAppId.find(target); found != m_byAppId.end()) {
        return m_entries[found->second];
    }
    for (const auto& app : m_entries) {
        if (targetMatchesBundle(app, target)) return app;
    }
    return std::nullopt;
}

} // namespace lcl::session
