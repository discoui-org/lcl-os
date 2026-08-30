#include "system/session/app_bundle_parser.hpp"

#include <dirent.h>
#include <unistd.h>

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMaxManifestBytes = 64 * 1024;
constexpr int kMaxJsonDepth = 32;

struct ManifestFields {
    std::optional<std::string> appId;
    std::optional<std::string> name;
    std::optional<std::string> version;
    std::optional<std::string> icon;
    std::optional<std::string> executable;
    std::optional<std::string> runtime;
    std::optional<std::string> type;
    std::vector<std::string> requestedPermissions;
};

bool appendUtf8(std::string& output, std::uint32_t codePoint) {
    if (codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
        return false;
    }

    if (codePoint <= 0x7F) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    return true;
}

class JsonManifestParser {
public:
    explicit JsonManifestParser(const std::string& input) : input_(input) {}

    bool parse(ManifestFields& fields) {
        skipWhitespace();
        if (!consume('{')) {
            return false;
        }

        std::unordered_set<std::string> keys;
        skipWhitespace();
        if (consume('}')) {
            return false;
        }

        while (true) {
            std::string key;
            if (!parseString(key) || !keys.insert(key).second) {
                return false;
            }
            skipWhitespace();
            if (!consume(':')) {
                return false;
            }
            skipWhitespace();

            if (key == "requestedPermissions") {
                if (!parseStringArray(fields.requestedPermissions)) {
                    return false;
                }
            } else if (isStringField(key)) {
                std::string value;
                if (!parseString(value) || !setStringField(fields, key, std::move(value))) {
                    return false;
                }
            } else if (!skipValue(0)) {
                return false;
            }

            skipWhitespace();
            if (consume('}')) {
                break;
            }
            if (!consume(',')) {
                return false;
            }
            skipWhitespace();
        }

        skipWhitespace();
        return position_ == input_.size();
    }

private:
    void skipWhitespace() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
    }

    bool consume(char expected) {
        if (position_ >= input_.size() || input_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    bool parseHexCodeUnit(std::uint32_t& value) {
        if (position_ + 4 > input_.size()) {
            return false;
        }
        value = 0;
        for (int index = 0; index < 4; ++index) {
            const char character = input_[position_++];
            value <<= 4;
            if (character >= '0' && character <= '9') {
                value |= static_cast<std::uint32_t>(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                value |= static_cast<std::uint32_t>(character - 'a' + 10);
            } else if (character >= 'A' && character <= 'F') {
                value |= static_cast<std::uint32_t>(character - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    bool parseString(std::string& output) {
        if (!consume('"')) {
            return false;
        }

        output.clear();
        while (position_ < input_.size()) {
            const char character = input_[position_++];
            if (character == '"') {
                return true;
            }
            if (static_cast<unsigned char>(character) < 0x20) {
                return false;
            }
            if (character != '\\') {
                output.push_back(character);
                continue;
            }

            if (position_ >= input_.size()) {
                return false;
            }
            switch (input_[position_++]) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': {
                    std::uint32_t codePoint = 0;
                    if (!parseHexCodeUnit(codePoint)) {
                        return false;
                    }
                    if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
                        if (position_ + 2 > input_.size() || input_[position_] != '\\' ||
                            input_[position_ + 1] != 'u') {
                            return false;
                        }
                        position_ += 2;
                        std::uint32_t lowSurrogate = 0;
                        if (!parseHexCodeUnit(lowSurrogate) || lowSurrogate < 0xDC00 ||
                            lowSurrogate > 0xDFFF) {
                            return false;
                        }
                        codePoint = 0x10000 + ((codePoint - 0xD800) << 10) +
                                    (lowSurrogate - 0xDC00);
                    } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
                        return false;
                    }
                    if (!appendUtf8(output, codePoint)) {
                        return false;
                    }
                    break;
                }
                default:
                    return false;
            }
        }
        return false;
    }

    bool parseStringArray(std::vector<std::string>& output) {
        if (!consume('[')) {
            return false;
        }
        output.clear();
        skipWhitespace();
        if (consume(']')) {
            return true;
        }

        while (true) {
            std::string value;
            if (!parseString(value)) {
                return false;
            }
            output.push_back(std::move(value));
            skipWhitespace();
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
            skipWhitespace();
        }
    }

    bool skipValue(int depth) {
        if (depth > kMaxJsonDepth) {
            return false;
        }
        skipWhitespace();
        if (position_ >= input_.size()) {
            return false;
        }

        if (input_[position_] == '"') {
            std::string discarded;
            return parseString(discarded);
        }
        if (input_[position_] == '{') {
            ++position_;
            skipWhitespace();
            if (consume('}')) {
                return true;
            }
            while (true) {
                std::string discardedKey;
                if (!parseString(discardedKey)) {
                    return false;
                }
                skipWhitespace();
                if (!consume(':') || !skipValue(depth + 1)) {
                    return false;
                }
                skipWhitespace();
                if (consume('}')) {
                    return true;
                }
                if (!consume(',')) {
                    return false;
                }
                skipWhitespace();
            }
        }
        if (input_[position_] == '[') {
            ++position_;
            skipWhitespace();
            if (consume(']')) {
                return true;
            }
            while (true) {
                if (!skipValue(depth + 1)) {
                    return false;
                }
                skipWhitespace();
                if (consume(']')) {
                    return true;
                }
                if (!consume(',')) {
                    return false;
                }
                skipWhitespace();
            }
        }
        if (consumeLiteral("true") || consumeLiteral("false") || consumeLiteral("null")) {
            return true;
        }
        return parseNumber();
    }

    bool consumeLiteral(std::string_view literal) {
        if (input_.compare(position_, literal.size(), literal) != 0) {
            return false;
        }
        position_ += literal.size();
        return true;
    }

    bool parseNumber() {
        const std::size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') {
            ++position_;
        }
        if (position_ >= input_.size()) {
            return false;
        }
        if (input_[position_] == '0') {
            ++position_;
        } else if (input_[position_] >= '1' && input_[position_] <= '9') {
            do {
                ++position_;
            } while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9');
        } else {
            return false;
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            const std::size_t fractionStart = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
            if (fractionStart == position_) {
                return false;
            }
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            const std::size_t exponentStart = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
            if (exponentStart == position_) {
                return false;
            }
        }
        return position_ != start;
    }

    static bool isStringField(const std::string& key) {
        return key == "id" || key == "name" || key == "version" || key == "icon" ||
               key == "executable" || key == "runtime" || key == "type";
    }

    static bool setStringField(ManifestFields& fields, const std::string& key, std::string value) {
        if (key == "id") fields.appId = std::move(value);
        else if (key == "name") fields.name = std::move(value);
        else if (key == "version") fields.version = std::move(value);
        else if (key == "icon") fields.icon = std::move(value);
        else if (key == "executable") fields.executable = std::move(value);
        else if (key == "runtime") fields.runtime = std::move(value);
        else if (key == "type") fields.type = std::move(value);
        else return false;
        return true;
    }

    const std::string& input_;
    std::size_t position_{0};
};

bool hasOnlyVisibleText(const std::string& value, std::size_t maximumLength) {
    if (value.empty() || value.size() > maximumLength || value.find('\0') != std::string::npos) {
        return false;
    }
    for (unsigned char character : value) {
        if (character < 0x20 || character == 0x7F) {
            return false;
        }
    }
    return true;
}

bool isAsciiLowerAlpha(char character) {
    return character >= 'a' && character <= 'z';
}

bool isAsciiLowerDigitOrHyphen(char character) {
    return isAsciiLowerAlpha(character) || (character >= '0' && character <= '9') ||
           character == '-';
}

bool isValidDottedIdentifier(const std::string& value, std::size_t maximumLength) {
    if (!hasOnlyVisibleText(value, maximumLength)) {
        return false;
    }

    bool labelStart = true;
    bool hasDot = false;
    char previous = '\0';
    for (char character : value) {
        if (character == '.') {
            if (labelStart || previous == '-') {
                return false;
            }
            hasDot = true;
            labelStart = true;
        } else {
            if (labelStart) {
                if (!isAsciiLowerAlpha(character)) {
                    return false;
                }
                labelStart = false;
            } else if (!isAsciiLowerDigitOrHyphen(character)) {
                return false;
            }
        }
        previous = character;
    }
    return hasDot && !labelStart && previous != '-';
}

bool isSafeBundleRelativePath(const std::string& value, std::string_view requiredFirstComponent) {
    if (!hasOnlyVisibleText(value, 512) || value.find('\\') != std::string::npos) {
        return false;
    }

    const fs::path path(value);
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return false;
    }

    auto component = path.begin();
    if (component == path.end() || component->string() != requiredFirstComponent) {
        return false;
    }
    for (; component != path.end(); ++component) {
        const std::string part = component->string();
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
    }
    return true;
}

bool isPathInside(const fs::path& directory, const fs::path& candidate) {
    auto directoryPart = directory.begin();
    auto candidatePart = candidate.begin();
    for (; directoryPart != directory.end() && candidatePart != candidate.end();
         ++directoryPart, ++candidatePart) {
        if (*directoryPart != *candidatePart) {
            return false;
        }
    }
    return directoryPart == directory.end() && candidatePart != candidate.end();
}

bool resolveRegularFileInsideBundle(const fs::path& bundleRoot, const std::string& relativePath,
                                    fs::path& resolvedPath) {
    std::error_code error;
    const fs::path candidate = fs::canonical(bundleRoot / relativePath, error);
    if (error || !isPathInside(bundleRoot, candidate)) {
        return false;
    }
    const fs::file_status status = fs::status(candidate, error);
    if (error || !fs::is_regular_file(status)) {
        return false;
    }
    resolvedPath = candidate;
    return true;
}

bool endsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

bool readManifest(const fs::path& manifestPath, std::string& content) {
    std::ifstream file(manifestPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return false;
    }
    const std::streamoff length = file.tellg();
    if (length < 0 || static_cast<std::uintmax_t>(length) > kMaxManifestBytes) {
        return false;
    }
    content.resize(static_cast<std::size_t>(length));
    file.seekg(0);
    return file.read(content.data(), length) || length == 0;
}

} // namespace

namespace lcl::core {

std::optional<AppBundleMetadata> AppBundleParser::parseBundle(const std::string& bundlePath) {
    std::error_code error;
    const fs::file_status suppliedStatus = fs::symlink_status(bundlePath, error);
    if (error || fs::is_symlink(suppliedStatus) || !fs::is_directory(suppliedStatus)) {
        return std::nullopt;
    }

    const fs::path bundleRoot = fs::canonical(bundlePath, error);
    if (error || !fs::is_directory(bundleRoot)) {
        return std::nullopt;
    }

    const fs::path manifestPath = bundleRoot / "Manifest.json";
    const fs::file_status manifestStatus = fs::symlink_status(manifestPath, error);
    if (error || fs::is_symlink(manifestStatus) || !fs::is_regular_file(manifestStatus)) {
        return std::nullopt;
    }

    const fs::path resourcesPath = fs::canonical(bundleRoot / "Resources", error);
    if (error || !isPathInside(bundleRoot, resourcesPath) || !fs::is_directory(resourcesPath)) {
        return std::nullopt;
    }
    const fs::path executablesPath = fs::canonical(bundleRoot / "Executables", error);
    if (error || !isPathInside(bundleRoot, executablesPath) || !fs::is_directory(executablesPath)) {
        return std::nullopt;
    }

    std::string content;
    if (!readManifest(manifestPath, content)) {
        return std::nullopt;
    }

    ManifestFields fields;
    JsonManifestParser parser(content);
    if (!parser.parse(fields) || !fields.appId || !fields.name || !fields.icon || !fields.executable ||
        !isValidDottedIdentifier(*fields.appId, 128) || !hasOnlyVisibleText(*fields.name, 256) ||
        !isSafeBundleRelativePath(*fields.icon, "Resources") ||
        !isSafeBundleRelativePath(*fields.executable, "Executables")) {
        return std::nullopt;
    }

    if (fields.version && !hasOnlyVisibleText(*fields.version, 128)) {
        return std::nullopt;
    }
    if (fields.runtime && !hasOnlyVisibleText(*fields.runtime, 128)) {
        return std::nullopt;
    }
    const std::string type = fields.type.value_or("gui");
    if (type != "gui" && type != "cli") {
        return std::nullopt;
    }

    std::unordered_set<std::string> permissions;
    for (const std::string& permission : fields.requestedPermissions) {
        if (!isValidDottedIdentifier(permission, 128) || !permissions.insert(permission).second) {
            return std::nullopt;
        }
    }

    fs::path iconPath;
    fs::path executablePath;
    if (!resolveRegularFileInsideBundle(bundleRoot, *fields.icon, iconPath) ||
        !resolveRegularFileInsideBundle(bundleRoot, *fields.executable, executablePath) ||
        !isPathInside(resourcesPath, iconPath) || !isPathInside(executablesPath, executablePath)) {
        return std::nullopt;
    }

    const bool isJavaScript = (fields.runtime && *fields.runtime == "org.lcl.javascript") ||
                              endsWith(executablePath.string(), ".js");
    const int requiredAccess = isJavaScript ? R_OK : X_OK;
    if (access(executablePath.c_str(), requiredAccess) != 0) {
        return std::nullopt;
    }

    AppBundleMetadata metadata{};
    metadata.bundlePath = bundleRoot.string();
    metadata.appId = *fields.appId;
    metadata.name = *fields.name;
    metadata.version = fields.version.value_or("");
    metadata.icon = *fields.icon;
    metadata.type = type;
    metadata.runtime = fields.runtime.value_or("");
    metadata.requestedPermissions = std::move(fields.requestedPermissions);
    metadata.executablePath = executablePath.string();
    metadata.valid = true;
    return metadata;
}

std::vector<AppBundleMetadata> AppBundleParser::scanDirectory(const std::string& searchDir) {
    std::vector<AppBundleMetadata> results;
    DIR* directory = opendir(searchDir.c_str());
    if (!directory) {
        return results;
    }

    while (dirent* entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name == "." || name == ".." || name.size() <= 4 ||
            name.substr(name.size() - 4) != ".app") {
            continue;
        }

        if (auto metadata = parseBundle((fs::path(searchDir) / name).string()); metadata && metadata->valid) {
            results.push_back(std::move(*metadata));
        }
    }
    closedir(directory);
    return results;
}

} // namespace lcl::core
