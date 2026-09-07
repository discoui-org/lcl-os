#include "system/security/permission_store.hpp"

#include "system/security/app_identity_registry.hpp"
#include "system/security/permission_policy.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <sstream>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace lcl::security {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kStoreHeader = "LCL_PERMISSION_STORE_V1";
constexpr std::size_t kMaximumStoreBytes = 1024 * 1024;
constexpr std::string_view kEd25519PublisherPrefix = "ed25519:";

class ScopedFd final {
public:
    explicit ScopedFd(int descriptor = -1) noexcept : descriptor_(descriptor) {}
    ~ScopedFd() {
        if (descriptor_ >= 0) close(descriptor_);
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ScopedFd(ScopedFd&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            if (descriptor_ >= 0) close(descriptor_);
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    int get() const noexcept { return descriptor_; }
    bool valid() const noexcept { return descriptor_ >= 0; }

private:
    int descriptor_{-1};
};

bool isLowerHex(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    });
}

bool isValidPublisherIdentity(std::string_view value) {
    if (value == "system-image" || value == "unverified") return true;
    return value.starts_with(kEd25519PublisherPrefix) &&
           value.size() == kEd25519PublisherPrefix.size() + 64 &&
           isLowerHex(value.substr(kEd25519PublisherPrefix.size()));
}

bool parseUid(std::string_view value, uid_t& output) {
    std::uintmax_t parsed = 0;
    const auto [end, conversionError] =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (conversionError != std::errc{} || end != value.data() + value.size() || parsed == 0 ||
        parsed > static_cast<std::uintmax_t>(std::numeric_limits<uid_t>::max())) {
        return false;
    }
    output = static_cast<uid_t>(parsed);
    return true;
}

bool parseNonzeroU32(std::string_view value, std::uint32_t& output) {
    std::uint32_t parsed = 0;
    const auto [end, conversionError] =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (conversionError != std::errc{} || end != value.data() + value.size() || parsed == 0) {
        return false;
    }
    output = parsed;
    return true;
}

bool decodeDigest(std::string_view text, Sha256Digest& digest) {
    if (text.size() != digest.size() * 2 || !isLowerHex(text)) return false;
    const auto decodeNibble = [](char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        return character - 'a' + 10;
    };
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const int high = decodeNibble(text[index * 2]);
        const int low = decodeNibble(text[index * 2 + 1]);
        digest[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return !isZeroDigest(digest);
}

bool validatePermission(std::string_view permission, std::string& error) {
    if (!isKnownPermission(permission)) {
        error = "permission identifier is invalid";
        return false;
    }
    return true;
}

bool sameSubject(const PermissionSubject& left, const PermissionSubject& right) {
    return left.userUid == right.userUid && left.appId == right.appId &&
           left.bundleRecordDigest == right.bundleRecordDigest &&
           left.publisherIdentity == right.publisherIdentity &&
           left.permissionVersion == right.permissionVersion;
}

bool sameDecisionKey(const PermissionDecision& left, const PermissionDecision& right) {
    return sameSubject(left.subject, right.subject) && left.permission == right.permission;
}

bool decisionLess(const PermissionDecision& left, const PermissionDecision& right) {
    if (left.subject.userUid != right.subject.userUid) {
        return left.subject.userUid < right.subject.userUid;
    }
    if (left.subject.appId != right.subject.appId) {
        return left.subject.appId < right.subject.appId;
    }
    if (left.subject.bundleRecordDigest != right.subject.bundleRecordDigest) {
        return left.subject.bundleRecordDigest < right.subject.bundleRecordDigest;
    }
    if (left.subject.publisherIdentity != right.subject.publisherIdentity) {
        return left.subject.publisherIdentity < right.subject.publisherIdentity;
    }
    if (left.subject.permissionVersion != right.subject.permissionVersion) {
        return left.subject.permissionVersion < right.subject.permissionVersion;
    }
    return left.permission < right.permission;
}

bool writeAll(int descriptor, std::string_view contents) {
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t count = write(descriptor, contents.data() + offset, contents.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool readAll(int descriptor, std::string& contents) {
    struct stat status {};
    if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
        static_cast<std::uintmax_t>(status.st_size) > kMaximumStoreBytes) {
        return false;
    }
    contents.resize(static_cast<std::size_t>(status.st_size));
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t count = read(descriptor, contents.data() + offset, contents.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool isSafeStoreFile(const fs::path& path, uid_t ownerUid, gid_t ownerGid, struct stat& status) {
    return lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) &&
           status.st_uid == ownerUid && status.st_gid == ownerGid &&
           (status.st_mode & 0077) == 0 && status.st_nlink == 1;
}

bool sameInode(const struct stat& left, const struct stat& right) {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

std::vector<std::string_view> splitFields(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start < line.size()) {
        const std::size_t separator = line.find(' ', start);
        fields.push_back(line.substr(start, separator - start));
        if (separator == std::string_view::npos) break;
        start = separator + 1;
    }
    return fields;
}

ScopedFd openStoreLock(const PermissionStoreConfig& config, int operation, std::string& error) {
    ScopedFd lock(open((config.storePath + ".lock").c_str(),
                       O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
    struct stat status {};
    if (!lock.valid() || fstat(lock.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != config.ownerUid || status.st_gid != config.ownerGid ||
        (status.st_mode & 0077) != 0 || status.st_nlink != 1) {
        error = "permission store lock has unsafe ownership, mode, link count, or type";
        return ScopedFd{};
    }
    if (flock(lock.get(), operation) != 0) {
        error = "could not acquire permission store lock";
        return ScopedFd{};
    }
    return lock;
}

bool normalizeRequestedPermissions(const std::vector<std::string>& input,
                                   std::vector<std::string>& output,
                                   std::string& error) {
    output = input;
    std::sort(output.begin(), output.end());
    for (std::size_t index = 0; index < output.size(); ++index) {
        if (!validatePermission(output[index], error) ||
            (index != 0 && output[index - 1] == output[index])) {
            if (error.empty()) error = "requested permissions are not canonical";
            return false;
        }
    }
    return true;
}

} // namespace

bool validatePermissionSubject(const PermissionSubject& subject, std::string& error) {
    if (subject.userUid == 0 || !AppIdentityRegistry::isValidAppId(subject.appId) ||
        isZeroDigest(subject.bundleRecordDigest) ||
        !isValidPublisherIdentity(subject.publisherIdentity) ||
        subject.permissionVersion == 0) {
        error = "permission decision subject is invalid";
        return false;
    }
    return true;
}

PermissionSubject makeSystemImagePermissionSubject(uid_t userUid, std::string appId,
                                                    const Sha256Digest& bundleRecordDigest) {
    return {.userUid = userUid,
            .appId = std::move(appId),
            .bundleRecordDigest = bundleRecordDigest,
            .publisherIdentity = "system-image",
            .permissionVersion = kPermissionDecisionVersion};
}

PermissionStore::PermissionStore(PermissionStoreConfig config) : config_(std::move(config)) {}

bool PermissionStore::validateConfig(std::string& error) const {
    if (config_.storePath.empty() || !fs::path(config_.storePath).is_absolute()) {
        error = "invalid permission store configuration";
        return false;
    }
    return true;
}

bool PermissionStore::validateStoreParent(std::string& error) const {
    const fs::path parent = fs::path(config_.storePath).parent_path();
    struct stat status {};
    if (parent.empty() || lstat(parent.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        S_ISLNK(status.st_mode) || status.st_uid != config_.ownerUid ||
        status.st_gid != config_.ownerGid || (status.st_mode & 0022) != 0) {
        error = "permission store parent is not owner-controlled";
        return false;
    }
    return true;
}

bool PermissionStore::loadUnlocked(std::string& error) {
    decisions_.clear();
    struct stat pathStatus {};
    if (lstat(config_.storePath.c_str(), &pathStatus) != 0) {
        if (errno == ENOENT) return true;
        error = std::string("could not inspect permission store: ") + std::strerror(errno);
        return false;
    }
    if (!isSafeStoreFile(config_.storePath, config_.ownerUid, config_.ownerGid, pathStatus)) {
        error = "permission store has unsafe ownership, mode, link count, or type";
        return false;
    }

    ScopedFd store(open(config_.storePath.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    struct stat openedStatus {};
    if (!store.valid() || fstat(store.get(), &openedStatus) != 0 ||
        !sameInode(pathStatus, openedStatus) || openedStatus.st_uid != config_.ownerUid ||
        openedStatus.st_gid != config_.ownerGid || (openedStatus.st_mode & 0077) != 0 ||
        openedStatus.st_nlink != 1) {
        error = "permission store changed while opening";
        return false;
    }

    std::string contents;
    if (!readAll(store.get(), contents)) {
        error = "could not read permission store";
        return false;
    }
    std::istringstream input(contents);
    std::string line;
    if (!std::getline(input, line) || line != kStoreHeader) {
        error = "permission store has an unknown format";
        return false;
    }
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const std::vector<std::string_view> fields = splitFields(line);
        PermissionDecision decision{};
        if (fields.size() != 7 || !parseUid(fields[0], decision.subject.userUid) ||
            !AppIdentityRegistry::isValidAppId(std::string(fields[1])) ||
            !decodeDigest(fields[2], decision.subject.bundleRecordDigest) ||
            !isValidPublisherIdentity(fields[3]) || !validatePermission(fields[4], error) ||
            !parseNonzeroU32(fields[5], decision.subject.permissionVersion) ||
            (fields[6] != "grant" && fields[6] != "deny")) {
            error = "permission store contains an invalid record";
            return false;
        }
        decision.subject.appId.assign(fields[1]);
        decision.subject.publisherIdentity.assign(fields[3]);
        decision.permission.assign(fields[4]);
        decision.granted = fields[6] == "grant";
        if (!validatePermissionSubject(decision.subject, error) ||
            std::any_of(decisions_.begin(), decisions_.end(),
                        [&decision](const PermissionDecision& existing) {
                            return sameDecisionKey(existing, decision);
                        })) {
            if (error.empty()) error = "permission store contains a duplicate record";
            return false;
        }
        decisions_.push_back(std::move(decision));
    }
    if (!std::is_sorted(decisions_.begin(), decisions_.end(), decisionLess)) {
        error = "permission store records are not canonical";
        return false;
    }
    return true;
}

bool PermissionStore::load(std::string& error) {
    error.clear();
    if (!validateConfig(error) || !validateStoreParent(error)) return false;
    ScopedFd lock = openStoreLock(config_, LOCK_SH, error);
    return lock.valid() && loadUnlocked(error);
}

bool PermissionStore::persistUnlocked(std::string& error) const {
    const fs::path storePath(config_.storePath);
    const fs::path parent = storePath.parent_path();
    const fs::path temporaryPath =
        parent / (storePath.filename().string() + ".tmp." + std::to_string(getpid()));
    unlink(temporaryPath.c_str());

    ScopedFd temporary(open(temporaryPath.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    struct stat temporaryStatus {};
    if (!temporary.valid() || fstat(temporary.get(), &temporaryStatus) != 0 ||
        ((temporaryStatus.st_uid != config_.ownerUid || temporaryStatus.st_gid != config_.ownerGid) &&
         fchown(temporary.get(), config_.ownerUid, config_.ownerGid) != 0) ||
        fchmod(temporary.get(), 0600) != 0) {
        error = "could not create a secure permission store transaction";
        unlink(temporaryPath.c_str());
        return false;
    }

    std::ostringstream output;
    output << kStoreHeader << '\n';
    for (const PermissionDecision& decision : decisions_) {
        output << decision.subject.userUid << ' ' << decision.subject.appId << ' '
               << hexEncodeDigest(decision.subject.bundleRecordDigest) << ' '
               << decision.subject.publisherIdentity << ' ' << decision.permission << ' '
               << decision.subject.permissionVersion << ' '
               << (decision.granted ? "grant" : "deny") << '\n';
    }
    if (!writeAll(temporary.get(), output.str()) || fsync(temporary.get()) != 0 ||
        rename(temporaryPath.c_str(), storePath.c_str()) != 0) {
        error = std::string("could not persist permission store transaction: ") +
                std::strerror(errno);
        unlink(temporaryPath.c_str());
        return false;
    }
    ScopedFd parentDescriptor(open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!parentDescriptor.valid() || fsync(parentDescriptor.get()) != 0) {
        error = "permission store transaction committed but parent directory could not be synced";
        return false;
    }
    return true;
}

bool PermissionStore::setDecision(const PermissionSubject& subject, std::string permission,
                                  bool granted, std::string& error) {
    error.clear();
    if (!validatePermissionSubject(subject, error) || !validatePermission(permission, error) ||
        !validateConfig(error) || !validateStoreParent(error)) {
        return false;
    }
    ScopedFd lock = openStoreLock(config_, LOCK_EX, error);
    if (!lock.valid() || !loadUnlocked(error)) return false;

    const PermissionDecision expected{subject, std::move(permission), granted};
    const std::vector<PermissionDecision> previous = decisions_;
    const auto existing = std::find_if(decisions_.begin(), decisions_.end(),
                                       [&expected](const PermissionDecision& decision) {
                                           return sameDecisionKey(decision, expected);
                                       });
    if (existing != decisions_.end()) {
        existing->granted = granted;
    } else {
        decisions_.push_back(expected);
    }
    std::sort(decisions_.begin(), decisions_.end(), decisionLess);
    if (!persistUnlocked(error)) {
        decisions_ = previous;
        return false;
    }
    return true;
}

bool PermissionStore::revoke(const PermissionSubject& subject, const std::string& permission,
                             std::string& error) {
    error.clear();
    if (!validatePermissionSubject(subject, error) || !validatePermission(permission, error) ||
        !validateConfig(error) || !validateStoreParent(error)) {
        return false;
    }
    ScopedFd lock = openStoreLock(config_, LOCK_EX, error);
    if (!lock.valid() || !loadUnlocked(error)) return false;

    const PermissionDecision expected{subject, permission, false};
    const std::vector<PermissionDecision> previous = decisions_;
    const auto newEnd = std::remove_if(decisions_.begin(), decisions_.end(),
                                       [&expected](const PermissionDecision& decision) {
                                           return sameDecisionKey(decision, expected);
                                       });
    if (newEnd == decisions_.end()) return true;
    decisions_.erase(newEnd, decisions_.end());
    if (!persistUnlocked(error)) {
        decisions_ = previous;
        return false;
    }
    return true;
}

bool PermissionStore::isGranted(const PermissionSubject& subject, const std::string& permission,
                                std::string& error) {
    error.clear();
    if (!validatePermissionSubject(subject, error) || !validatePermission(permission, error) || !load(error)) {
        return false;
    }
    const PermissionDecision expected{subject, permission, false};
    const auto decision = std::find_if(decisions_.begin(), decisions_.end(),
                                       [&expected](const PermissionDecision& existing) {
                                           return sameDecisionKey(existing, expected);
                                       });
    return decision != decisions_.end() && decision->granted;
}

std::vector<std::string> PermissionStore::grantedPermissions(
    const PermissionSubject& subject, const std::vector<std::string>& requested, std::string& error) {
    error.clear();
    std::vector<std::string> canonicalRequested;
    if (!validatePermissionSubject(subject, error) ||
        !normalizeRequestedPermissions(requested, canonicalRequested, error) || !load(error)) {
        return {};
    }
    std::vector<std::string> granted;
    for (const PermissionDecision& decision : decisions_) {
        if (decision.granted && sameSubject(decision.subject, subject) &&
            std::binary_search(canonicalRequested.begin(), canonicalRequested.end(), decision.permission)) {
            granted.push_back(decision.permission);
        }
    }
    return granted;
}

} // namespace lcl::security
