#include "system/session/app_bundle_parser.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cctype>
#include <cstdint>
#include <filesystem>
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
constexpr std::size_t kMaxSignatureEnvelopeBytes = 64 * 1024;
constexpr int kMaxJsonDepth = 32;

class ScopedFd final {
public:
    explicit ScopedFd(int descriptor = -1) noexcept : descriptor_(descriptor) {}
    ~ScopedFd() {
        if (descriptor_ >= 0) {
            close(descriptor_);
        }
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    ScopedFd(ScopedFd&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            if (descriptor_ >= 0) {
                close(descriptor_);
            }
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    int get() const noexcept { return descriptor_; }
    bool valid() const noexcept { return descriptor_ >= 0; }
    int release() noexcept { return std::exchange(descriptor_, -1); }

private:
    int descriptor_{-1};
};

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
    if (!hasOnlyVisibleText(value, 512) || value.find('\\') != std::string::npos ||
        value.find("//") != std::string::npos) {
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

bool isRegularFileDescriptor(int descriptor, struct stat& status) {
    return descriptor >= 0 && fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode) &&
           status.st_nlink == 1;
}

ScopedFd openDirectoryAt(int parentDescriptor, const char* name) {
    const int descriptor = openat(parentDescriptor, name,
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) {
        return {};
    }
    struct stat status {};
    if (fstat(descriptor, &status) != 0 || !S_ISDIR(status.st_mode)) {
        close(descriptor);
        return {};
    }
    return ScopedFd(descriptor);
}

std::vector<std::string> splitRelativePath(const std::string& path) {
    std::vector<std::string> components;
    std::size_t start = 0;
    while (start < path.size()) {
        const std::size_t end = path.find('/', start);
        components.push_back(path.substr(start, end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return components;
}

ScopedFd openRegularFileAt(int bundleDescriptor, const std::string& relativePath,
                           struct stat& fileStatus) {
    const std::vector<std::string> components = splitRelativePath(relativePath);
    if (components.empty()) {
        return {};
    }

    int parentDescriptor = bundleDescriptor;
    ScopedFd ownedParent;
    for (std::size_t index = 0; index < components.size(); ++index) {
        const bool finalComponent = index + 1 == components.size();
        const int flags = finalComponent
            ? O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK
            : O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
        const int descriptor = openat(parentDescriptor, components[index].c_str(), flags);
        if (descriptor < 0) {
            return {};
        }

        if (finalComponent) {
            if (!isRegularFileDescriptor(descriptor, fileStatus)) {
                close(descriptor);
                return {};
            }
            return ScopedFd(descriptor);
        }

        struct stat directoryStatus {};
        if (fstat(descriptor, &directoryStatus) != 0 || !S_ISDIR(directoryStatus.st_mode)) {
            close(descriptor);
            return {};
        }
        ownedParent = ScopedFd(descriptor);
        parentDescriptor = ownedParent.get();
    }
    return {};
}

ScopedFd openOptionalSignatureEnvelopeAt(int bundleDescriptor, struct stat& fileStatus,
                                         bool& wasPresent) {
    wasPresent = false;
    const int descriptor = openat(bundleDescriptor, "Signature.ed25519",
                                  O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) {
        return {};
    }
    wasPresent = true;
    if (!isRegularFileDescriptor(descriptor, fileStatus) || fileStatus.st_size < 0 ||
        static_cast<std::uintmax_t>(fileStatus.st_size) > kMaxSignatureEnvelopeBytes) {
        close(descriptor);
        return {};
    }
    return ScopedFd(descriptor);
}

bool endsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

bool readManifest(int descriptor, std::string& content) {
    struct stat status {};
    if (!isRegularFileDescriptor(descriptor, status) || status.st_size < 0 ||
        static_cast<std::uintmax_t>(status.st_size) > kMaxManifestBytes || lseek(descriptor, 0, SEEK_SET) < 0) {
        return false;
    }
    content.resize(static_cast<std::size_t>(status.st_size));
    std::size_t offset = 0;
    while (offset < content.size()) {
        const ssize_t readCount = read(descriptor, content.data() + offset, content.size() - offset);
        if (readCount > 0) {
            offset += static_cast<std::size_t>(readCount);
            continue;
        }
        if (readCount < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

} // namespace

namespace lcl::core {

AppBundleFileHandle::AppBundleFileHandle(int descriptor) noexcept : m_descriptor(descriptor) {}

AppBundleFileHandle::~AppBundleFileHandle() {
    if (m_descriptor >= 0) {
        close(m_descriptor);
    }
}

std::optional<AppBundleMetadata> AppBundleParser::parseBundle(const std::string& bundlePath) {
    ScopedFd bundleDescriptor(open(bundlePath.c_str(),
                                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat bundleStatus {};
    if (!bundleDescriptor.valid() || fstat(bundleDescriptor.get(), &bundleStatus) != 0 ||
        !S_ISDIR(bundleStatus.st_mode)) {
        return std::nullopt;
    }

    ScopedFd manifestDescriptor(openat(bundleDescriptor.get(), "Manifest.json",
                                       O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    std::string content;
    if (!readManifest(manifestDescriptor.get(), content)) {
        return std::nullopt;
    }

    ScopedFd resourcesDescriptor = openDirectoryAt(bundleDescriptor.get(), "Resources");
    ScopedFd executablesDescriptor = openDirectoryAt(bundleDescriptor.get(), "Executables");
    if (!resourcesDescriptor.valid() || !executablesDescriptor.valid()) {
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

    struct stat iconStatus {};
    struct stat executableStatus {};
    ScopedFd iconDescriptor = openRegularFileAt(bundleDescriptor.get(), *fields.icon, iconStatus);
    ScopedFd executableDescriptor =
        openRegularFileAt(bundleDescriptor.get(), *fields.executable, executableStatus);
    if (!iconDescriptor.valid() || !executableDescriptor.valid()) {
        return std::nullopt;
    }

    struct stat signatureStatus {};
    bool signatureEnvelopePresent = false;
    ScopedFd signatureDescriptor =
        openOptionalSignatureEnvelopeAt(bundleDescriptor.get(), signatureStatus, signatureEnvelopePresent);
    if (!signatureDescriptor.valid() && signatureEnvelopePresent) {
        return std::nullopt;
    }
    if (!signatureDescriptor.valid() && errno != ENOENT) {
        return std::nullopt;
    }

    const bool isJavaScript = (fields.runtime && *fields.runtime == "org.lcl.javascript") ||
                              endsWith(*fields.executable, ".js");
    if (!isJavaScript && (executableStatus.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0) {
        return std::nullopt;
    }

    std::error_code error;
    const fs::path displayBundlePath = fs::absolute(bundlePath, error).lexically_normal();
    if (error) {
        return std::nullopt;
    }

    AppBundleMetadata metadata{};
    metadata.bundlePath = displayBundlePath.string();
    metadata.appId = *fields.appId;
    metadata.name = *fields.name;
    metadata.version = fields.version.value_or("");
    metadata.icon = *fields.icon;
    metadata.executable = *fields.executable;
    metadata.manifestContents = std::move(content);
    metadata.type = type;
    metadata.runtime = fields.runtime.value_or("");
    metadata.requestedPermissions = std::move(fields.requestedPermissions);
    metadata.bundleHandle = std::make_shared<AppBundleFileHandle>(bundleDescriptor.release());
    metadata.manifestHandle = std::make_shared<AppBundleFileHandle>(manifestDescriptor.release());
    if (signatureDescriptor.valid()) {
        metadata.signatureHandle = std::make_shared<AppBundleFileHandle>(signatureDescriptor.release());
    }
    metadata.iconHandle = std::make_shared<AppBundleFileHandle>(iconDescriptor.release());
    metadata.executableHandle = std::make_shared<AppBundleFileHandle>(executableDescriptor.release());
    metadata.executablePath = (displayBundlePath / *fields.executable).string();
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
