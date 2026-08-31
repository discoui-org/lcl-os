#include "system/security/pending_bundle_approval_store.hpp"

#include "system/security/app_identity_registry.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
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

constexpr std::string_view kStoreHeader = "LCL_PENDING_BUNDLE_APPROVALS_V1";
constexpr std::size_t kMaximumStoreBytes = 1024 * 1024;
constexpr std::size_t kMaximumBundlePathBytes = 4096;
constexpr std::size_t kMaximumDisplayNameBytes = 256;
constexpr std::size_t kMaximumVersionBytes = 128;

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

private:
    int descriptor_{-1};
};

bool isSafeStoreFile(const fs::path& path, uid_t ownerUid, gid_t ownerGid,
                     struct stat& status) {
    return lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) &&
           status.st_uid == ownerUid && status.st_gid == ownerGid &&
           (status.st_mode & 0077) == 0 && status.st_nlink == 1;
}

bool writeAll(int descriptor, std::string_view contents) {
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t count = write(descriptor, contents.data() + offset,
                                    contents.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
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
        const ssize_t count = read(descriptor, contents.data() + offset,
                                   contents.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
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

bool decodeDigest(std::string_view text, Sha256Digest& digest) {
    if (text.size() != digest.size() * 2) {
        return false;
    }
    const auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const int high = nibble(text[index * 2]);
        const int low = nibble(text[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        digest[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return !isZeroDigest(digest);
}

bool parseSourceScope(std::string_view value, BundleSourceScope& scope) {
    if (value == "machine") {
        scope = BundleSourceScope::Machine;
        return true;
    }
    if (value == "user") {
        scope = BundleSourceScope::User;
        return true;
    }
    if (value == "direct") {
        scope = BundleSourceScope::Direct;
        return true;
    }
    return false;
}

std::string_view sourceScopeText(BundleSourceScope scope) {
    switch (scope) {
        case BundleSourceScope::Machine: return "machine";
        case BundleSourceScope::User: return "user";
        case BundleSourceScope::Direct: return "direct";
        case BundleSourceScope::System: return "system";
    }
    return {};
}

bool hasSafeVisibleText(std::string_view value, std::size_t maximumLength,
                        bool permitEmpty = false) {
    if ((!permitEmpty && value.empty()) || value.size() > maximumLength) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return character >= 0x20 && character != 0x7F;
    });
}

bool isSafeBundlePath(std::string_view value) {
    return hasSafeVisibleText(value, kMaximumBundlePathBytes) &&
           fs::path(std::string(value)).is_absolute();
}

std::string hexEncodeText(std::string_view value) {
    if (value.empty()) {
        return "-";
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2);
    for (const unsigned char character : value) {
        result.push_back(kHex[character >> 4]);
        result.push_back(kHex[character & 0x0F]);
    }
    return result;
}

bool hexDecodeText(std::string_view encoded, std::string& value) {
    value.clear();
    if (encoded == "-") {
        return true;
    }
    if (encoded.empty() || encoded.size() % 2 != 0) {
        return false;
    }
    const auto nibble = [](char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        return -1;
    };
    value.reserve(encoded.size() / 2);
    for (std::size_t index = 0; index < encoded.size(); index += 2) {
        const int high = nibble(encoded[index]);
        const int low = nibble(encoded[index + 1]);
        if (high < 0 || low < 0) {
            value.clear();
            return false;
        }
        value.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

std::vector<std::string_view> splitFields(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start < line.size()) {
        const std::size_t separator = line.find(' ', start);
        fields.push_back(line.substr(start, separator - start));
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1;
    }
    return fields;
}

bool sameRequestIdentity(const PendingBundleApproval& left,
                         const PendingBundleApproval& right) {
    return left.userUid == right.userUid && left.appId == right.appId &&
           left.bundleRecordDigest == right.bundleRecordDigest &&
           left.publisherState == right.publisherState;
}

bool sameRequest(const PendingBundleApproval& left, const PendingBundleApproval& right) {
    return sameRequestIdentity(left, right) && left.sourceScope == right.sourceScope &&
           left.bundlePath == right.bundlePath && left.displayName == right.displayName &&
           left.appVersion == right.appVersion;
}

bool validatePendingRequest(const PendingBundleApproval& request, std::string& error) {
    if (request.userUid == 0 || !AppIdentityRegistry::isValidAppId(request.appId) ||
        isZeroDigest(request.bundleRecordDigest) ||
        request.publisherState != BundlePublisherState::Unverified ||
        request.sourceScope == BundleSourceScope::System || !isSafeBundlePath(request.bundlePath) ||
        !hasSafeVisibleText(request.displayName, kMaximumDisplayNameBytes) ||
        !hasSafeVisibleText(request.appVersion, kMaximumVersionBytes, true)) {
        error = "pending bundle approval request is invalid";
        return false;
    }
    return true;
}

ScopedFd openStoreLock(const PendingBundleApprovalStoreConfig& config, int operation,
                       std::string& error) {
    ScopedFd lock(open((config.storePath + ".lock").c_str(),
                       O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
    struct stat status {};
    if (!lock.valid() || fstat(lock.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != config.ownerUid || status.st_gid != config.ownerGid ||
        (status.st_mode & 0077) != 0 || status.st_nlink != 1) {
        error = "pending bundle approval lock has unsafe ownership, mode, link count, or type";
        return ScopedFd{};
    }
    if (flock(lock.get(), operation) != 0) {
        error = "could not acquire pending bundle approval lock";
        return ScopedFd{};
    }
    return lock;
}

} // namespace

PendingBundleApprovalStore::PendingBundleApprovalStore(PendingBundleApprovalStoreConfig config)
    : config_(std::move(config)) {}

bool PendingBundleApprovalStore::validateConfig(std::string& error) const {
    if (config_.storePath.empty() || !fs::path(config_.storePath).is_absolute()) {
        error = "invalid pending bundle approval store configuration";
        return false;
    }
    return true;
}

bool PendingBundleApprovalStore::validateStoreParent(std::string& error) const {
    const fs::path parent = fs::path(config_.storePath).parent_path();
    struct stat status {};
    if (parent.empty() || lstat(parent.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        S_ISLNK(status.st_mode) || status.st_uid != config_.ownerUid ||
        status.st_gid != config_.ownerGid || (status.st_mode & 0022) != 0) {
        error = "pending bundle approval store parent is not owner-controlled";
        return false;
    }
    return true;
}

bool PendingBundleApprovalStore::loadUnlocked(std::string& error) {
    pending_.clear();
    struct stat pathStatus {};
    if (lstat(config_.storePath.c_str(), &pathStatus) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        error = std::string("could not inspect pending bundle approval store: ") +
                std::strerror(errno);
        return false;
    }
    if (!isSafeStoreFile(config_.storePath, config_.ownerUid, config_.ownerGid, pathStatus)) {
        error = "pending bundle approval store has unsafe ownership, mode, link count, or type";
        return false;
    }
    ScopedFd store(open(config_.storePath.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    struct stat openedStatus {};
    if (!store.valid() || fstat(store.get(), &openedStatus) != 0 ||
        openedStatus.st_dev != pathStatus.st_dev || openedStatus.st_ino != pathStatus.st_ino ||
        openedStatus.st_uid != config_.ownerUid || openedStatus.st_gid != config_.ownerGid ||
        (openedStatus.st_mode & 0077) != 0 || openedStatus.st_nlink != 1) {
        error = "pending bundle approval store changed while opening";
        return false;
    }
    std::string contents;
    if (!readAll(store.get(), contents)) {
        error = "could not read pending bundle approval store";
        return false;
    }
    std::istringstream input(contents);
    std::string line;
    if (!std::getline(input, line) || line != kStoreHeader) {
        error = "pending bundle approval store has an unknown format";
        return false;
    }
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        const std::vector<std::string_view> fields = splitFields(line);
        PendingBundleApproval request{};
        if (fields.size() != 8 || !parseUid(fields[0], request.userUid) ||
            !AppIdentityRegistry::isValidAppId(std::string(fields[1])) ||
            !decodeDigest(fields[2], request.bundleRecordDigest) || fields[3] != "unverified" ||
            !parseSourceScope(fields[4], request.sourceScope) ||
            !hexDecodeText(fields[5], request.bundlePath) ||
            !hexDecodeText(fields[6], request.displayName) ||
            !hexDecodeText(fields[7], request.appVersion)) {
            error = "pending bundle approval store contains an invalid record";
            return false;
        }
        request.appId.assign(fields[1]);
        request.publisherState = BundlePublisherState::Unverified;
        if (!validatePendingRequest(request, error) ||
            std::any_of(pending_.begin(), pending_.end(), [&request](const auto& existing) {
                return sameRequestIdentity(existing, request);
            })) {
            if (error.empty()) {
                error = "pending bundle approval store contains a duplicate record";
            }
            return false;
        }
        pending_.push_back(std::move(request));
    }
    return true;
}

bool PendingBundleApprovalStore::persistUnlocked(std::string& error) const {
    const fs::path storePath(config_.storePath);
    const fs::path parent = storePath.parent_path();
    const fs::path temporaryPath =
        parent / (storePath.filename().string() + ".tmp." + std::to_string(getpid()));
    unlink(temporaryPath.c_str());
    ScopedFd temporary(open(temporaryPath.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    struct stat temporaryStatus {};
    if (!temporary.valid() || fstat(temporary.get(), &temporaryStatus) != 0 ||
        ((temporaryStatus.st_uid != config_.ownerUid ||
          temporaryStatus.st_gid != config_.ownerGid) &&
         fchown(temporary.get(), config_.ownerUid, config_.ownerGid) != 0) ||
        fchmod(temporary.get(), 0600) != 0) {
        error = "could not create a secure pending bundle approval transaction";
        unlink(temporaryPath.c_str());
        return false;
    }
    std::ostringstream output;
    output << kStoreHeader << '\n';
    for (const PendingBundleApproval& request : pending_) {
        output << request.userUid << ' ' << request.appId << ' '
               << hexEncodeDigest(request.bundleRecordDigest) << " unverified "
               << sourceScopeText(request.sourceScope) << ' '
               << hexEncodeText(request.bundlePath) << ' '
               << hexEncodeText(request.displayName) << ' '
               << hexEncodeText(request.appVersion) << '\n';
    }
    if (!writeAll(temporary.get(), output.str()) || fsync(temporary.get()) != 0 ||
        rename(temporaryPath.c_str(), storePath.c_str()) != 0) {
        error = std::string("could not persist pending bundle approval transaction: ") +
                std::strerror(errno);
        unlink(temporaryPath.c_str());
        return false;
    }
    ScopedFd parentDescriptor(open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!parentDescriptor.valid() || fsync(parentDescriptor.get()) != 0) {
        error = "pending bundle approval transaction committed but parent directory could not be synced";
        return false;
    }
    return true;
}

bool PendingBundleApprovalStore::record(uid_t userUid, const BundleRecord& record,
                                        BundlePublisherState publisherState,
                                        BundleSourceScope sourceScope, std::string bundlePath,
                                        std::string displayName, std::string& error) {
    error.clear();
    if (!validateBundleRecord(record, error) || publisherState != BundlePublisherState::Unverified ||
        !validateConfig(error) || !validateStoreParent(error)) {
        if (error.empty()) {
            error = "only an unverified direct bundle may await user approval";
        }
        return false;
    }
    PendingBundleApproval request{
        .userUid = userUid,
        .appId = record.appId,
        .bundleRecordDigest = record.digest,
        .publisherState = publisherState,
        .sourceScope = sourceScope,
        .bundlePath = std::move(bundlePath),
        .displayName = std::move(displayName),
        .appVersion = record.appVersion,
    };
    if (!validatePendingRequest(request, error)) {
        return false;
    }
    ScopedFd lock = openStoreLock(config_, LOCK_EX, error);
    if (!lock.valid() || !loadUnlocked(error)) {
        return false;
    }
    const auto existing = std::find_if(pending_.begin(), pending_.end(), [&request](const auto& value) {
        return sameRequestIdentity(value, request);
    });
    if (existing != pending_.end()) {
        if (sameRequest(*existing, request)) {
            return true;
        }
        const PendingBundleApproval previous = *existing;
        *existing = std::move(request);
        if (!persistUnlocked(error)) {
            *existing = previous;
            return false;
        }
        return true;
    }
    pending_.push_back(std::move(request));
    if (!persistUnlocked(error)) {
        pending_.pop_back();
        return false;
    }
    return true;
}

bool PendingBundleApprovalStore::remove(uid_t userUid, const BundleRecord& record,
                                        BundlePublisherState publisherState, std::string& error) {
    error.clear();
    if (userUid == 0 || publisherState != BundlePublisherState::Unverified ||
        !validateBundleRecord(record, error)) {
        if (error.empty()) {
            error = "only an unverified bundle approval request may be removed";
        }
        return false;
    }
    return removeExactUnverified(userUid, record.appId, record.digest, error);
}

bool PendingBundleApprovalStore::removeExactUnverified(
    uid_t userUid, const std::string& appId, const Sha256Digest& bundleRecordDigest,
    std::string& error) {
    error.clear();
    if (userUid == 0 || !AppIdentityRegistry::isValidAppId(appId) ||
        isZeroDigest(bundleRecordDigest) || !validateConfig(error) ||
        !validateStoreParent(error)) {
        if (error.empty()) {
            error = "an exact unverified pending bundle identity is required";
        }
        return false;
    }
    ScopedFd lock = openStoreLock(config_, LOCK_EX, error);
    if (!lock.valid() || !loadUnlocked(error)) {
        return false;
    }
    const auto hasIdentity = [userUid, &appId, &bundleRecordDigest](const auto& value) {
        return value.userUid == userUid && value.appId == appId &&
               value.bundleRecordDigest == bundleRecordDigest &&
               value.publisherState == BundlePublisherState::Unverified;
    };
    const auto newEnd = std::remove_if(pending_.begin(), pending_.end(), hasIdentity);
    if (newEnd == pending_.end()) {
        return true;
    }
    pending_.erase(newEnd, pending_.end());
    return persistUnlocked(error);
}

std::vector<PendingBundleApproval> PendingBundleApprovalStore::pendingFor(
    uid_t userUid, std::string& error) {
    error.clear();
    if (userUid == 0 || !validateConfig(error) || !validateStoreParent(error)) {
        if (error.empty()) {
            error = "pending bundle approvals require a non-root user";
        }
        return {};
    }
    ScopedFd lock = openStoreLock(config_, LOCK_SH, error);
    if (!lock.valid() || !loadUnlocked(error)) {
        return {};
    }
    std::vector<PendingBundleApproval> result;
    for (const PendingBundleApproval& request : pending_) {
        if (request.userUid == userUid) {
            result.push_back(request);
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.appId != right.appId) {
            return left.appId < right.appId;
        }
        return left.bundleRecordDigest < right.bundleRecordDigest;
    });
    return result;
}

} // namespace lcl::security
