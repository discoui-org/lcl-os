#include "system/session/app_bundle_parser.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// Helper to extract JSON string value for a key e.g. "name": "System Monitor"
std::string extractJsonString(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return "";

    size_t colon = json.find(':', pos);
    if (colon == std::string::npos) return "";

    size_t startQuote = json.find('"', colon);
    if (startQuote == std::string::npos) return "";

    size_t endQuote = json.find('"', startQuote + 1);
    if (endQuote == std::string::npos) return "";

    return json.substr(startQuote + 1, endQuote - startQuote - 1);
}

} // namespace

namespace lcl::core {

std::optional<AppBundleMetadata> AppBundleParser::parseBundle(const std::string& bundlePath) {
    struct stat st{};
    if (stat(bundlePath.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        return std::nullopt;
    }

    std::string base = bundlePath;
    if (base.back() != '/') base += "/";

    std::string manifestPath = base + "Manifest.json";
    const std::string resourcesPath = base + "Resources";
    struct stat resourcesStat{};
    if (stat(resourcesPath.c_str(), &resourcesStat) != 0 ||
        !S_ISDIR(resourcesStat.st_mode)) {
        return std::nullopt;
    }

    std::ifstream file(manifestPath);
    if (!file.is_open()) {
        return std::nullopt;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    AppBundleMetadata meta{};
    meta.bundlePath = bundlePath;
    meta.appId = extractJsonString(content, "id");
    meta.name = extractJsonString(content, "name");
    meta.version = extractJsonString(content, "version");
    meta.icon = extractJsonString(content, "icon");
    meta.runtime = extractJsonString(content, "runtime");
    meta.type = extractJsonString(content, "type");
    if (meta.type.empty()) {
        meta.type = "gui";
    }

    std::string execRel = extractJsonString(content, "executable");
    if (meta.appId.empty() || execRel.empty() || meta.name.empty() ||
        meta.icon.rfind("Resources/", 0) != 0 ||
        meta.icon.find("..") != std::string::npos) {
        return std::nullopt;
    }

    struct stat iconStat{};
    const std::string iconPath = base + meta.icon;
    if (stat(iconPath.c_str(), &iconStat) != 0 || !S_ISREG(iconStat.st_mode)) {
        return std::nullopt;
    }

    // Resolve full executable path
    if (execRel[0] == '/') {
        meta.executablePath = execRel;
    } else {
        meta.executablePath = base + execRel;
    }

    meta.valid = true;
    return meta;
}

std::vector<AppBundleMetadata> AppBundleParser::scanDirectory(const std::string& searchDir) {
    std::vector<AppBundleMetadata> results;
    DIR* dir = opendir(searchDir.c_str());
    if (!dir) return results;

    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        if (name.size() > 4 && name.substr(name.size() - 4) == ".app") {
            std::string fullPath = searchDir;
            if (fullPath.back() != '/') fullPath += "/";
            fullPath += name;

            auto meta = parseBundle(fullPath);
            if (meta && meta->valid) {
                results.push_back(*meta);
            }
        }
    }
    closedir(dir);
    return results;
}

} // namespace lcl::core
