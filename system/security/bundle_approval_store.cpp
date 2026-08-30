#include "system/security/bundle_approval_store.hpp"

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
#include <vector>

namespace lcl::security {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kStoreHeader = "LCL_BUNDLE_APPROVALS_V1";
constexpr std::size_t kMaximumStoreBytes = 1024 * 1024;

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

bool isSafeStoreFile(const fs::path& path, uid_t ownerUid, gid_t ownerGid, struct stat& status) {
    return lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) &&
           status.st_uid == ownerUid && status.st_gid == ownerGid && (status.st_mode & 0077) == 0 &&
           status.st_nlink == 1;
}

bool writeAll(int descriptor, std::string_view contents) {
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t count = write(descriptor, contents.data() + offset, contents.size() - offset);
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
        const ssize_t count = read(descriptor, contents.data() + offset, contents.size() - offset);
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
    const auto [end, conversionError] = std::from_chars(value.data(), value.data() + value.size(), parsed);
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
    const auto decodeNibble = [](char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const int high = decodeNibble(text[index * 2]);
        const int low = decodeNibble(text[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        digest[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return !isZeroDigest(digest);
}

bool parsePublisherState(std::string_view text, BundlePublisherState& state) {
    if (text == "unverified") {
        state = BundlePublisherState::Unverified;
        return true;
    }
    if (text == "signature-verified") {
        state = BundlePublisherState::SignatureVerified;
        return true;
    }
    if (text == "system-image-trusted") {
        state = BundlePublisherState::SystemImageTrusted;
        return true;
    }
    return false;
}

std::string_view publisherStateText(BundlePublisherState state) {
    switch (state) {
        case BundlePublisherState::Unverified: return "unverified";
        case BundlePublisherState::SignatureVerified: return "signature-verified";
        case BundlePublisherState::SystemImageTrusted: return "system-image-trusted";
    }
    return "";
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

bool sameApproval(const BundleApproval& left, const BundleApproval& right) {
    return left.userUid == right.userUid && left.appId == right.appId &&
           left.bundleRecordDigest == right.bundleRecordDigest &&
           left.publisherState == right.publisherState;
}

bool sameInode(const struct stat& left, const struct stat& right) {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

ScopedFd openStoreLock(const BundleApprovalStoreConfig& config, int lockOperation, std::string& error) {
    ScopedFd lock(open((config.storePath + ".lock").c_str(),
                       O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
    struct stat status {};
    if (!lock.valid() || fstat(lock.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != config.ownerUid || status.st_gid != config.ownerGid ||
        (status.st_mode & 0077) != 0 || status.st_nlink != 1) {
        error = "bundle approval lock has unsafe ownership, mode, link count, or type";
        return {};
    }
    if (flock(lock.get(), lockOperation) != 0) {
        error = "could not acquire bundle approval lock";
        return {};
    }
    return lock;
}

} // namespace

BundleApprovalStore::BundleApprovalStore(BundleApprovalStoreConfig config) : config_(std::move(config)) {}

bool BundleApprovalStore::validateConfig(std::string& error) const {
    if (config_.storePath.empty() || !fs::path(config_.storePath).is_absolute()) {
        error = "invalid bundle approval store configuration";
        return false;
    }
    return true;
}

bool BundleApprovalStore::validateStoreParent(std::string& error) const {
    const fs::path parent = fs::path(config_.storePath).parent_path();
    struct stat status {};
    if (parent.empty() || lstat(parent.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        S_ISLNK(status.st_mode) || status.st_uid != config_.ownerUid || status.st_gid != config_.ownerGid ||
        (status.st_mode & 0022) != 0) {
        error = "bundle approval store parent is not owner-controlled";
        return false;
    }
    return true;
}

bool BundleApprovalStore::loadUnlocked(std::string& error) {
    approvals_.clear();
    struct stat pathStatus {};
    if (lstat(config_.storePath.c_str(), &pathStatus) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        error = std::string("could not inspect bundle approval store: ") + std::strerror(errno);
        return false;
    }
    if (!isSafeStoreFile(config_.storePath, config_.ownerUid, config_.ownerGid, pathStatus)) {
        error = "bundle approval store has unsafe ownership, mode, link count, or type";
        return false;
    }

    ScopedFd store(open(config_.storePath.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    struct stat openedStatus {};
    if (!store.valid() || fstat(store.get(), &openedStatus) != 0 || !sameInode(pathStatus, openedStatus) ||
        openedStatus.st_uid != config_.ownerUid || openedStatus.st_gid != config_.ownerGid ||
        (openedStatus.st_mode & 0077) != 0 || openedStatus.st_nlink != 1) {
        error = "bundle approval store changed while opening";
        return false;
    }

    std::string contents;
    if (!readAll(store.get(), contents)) {
        error = "could not read bundle approval store";
        return false;
    }
    std::istringstream input(contents);
    std::string line;
    if (!std::getline(input, line) || line != kStoreHeader) {
        error = "bundle approval store has an unknown format";
        return false;
    }
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        const std::vector<std::string_view> fields = splitFields(line);
        BundleApproval approval{};
        if (fields.size() != 4 || !parseUid(fields[0], approval.userUid) ||
            !AppIdentityRegistry::isValidAppId(std::string(fields[1])) ||
            !decodeDigest(fields[2], approval.bundleRecordDigest) ||
            !parsePublisherState(fields[3], approval.publisherState)) {
            error = "bundle approval store contains an invalid record";
            return false;
        }
        approval.appId.assign(fields[1]);
        if (approval.publisherState != BundlePublisherState::Unverified ||
            std::any_of(approvals_.begin(), approvals_.end(),
                        [&approval](const BundleApproval& existing) { return sameApproval(existing, approval); })) {
            error = "bundle approval store contains a duplicate or forbidden record";
            return false;
        }
        approvals_.push_back(std::move(approval));
    }
    return true;
}

bool BundleApprovalStore::load(std::string& error) {
    error.clear();
    if (!validateConfig(error) || !validateStoreParent(error)) {
        return false;
    }
    ScopedFd lock = openStoreLock(config_, LOCK_SH, error);
    return lock.valid() && loadUnlocked(error);
}

bool BundleApprovalStore::persistUnlocked(std::string& error) const {
    const fs::path storePath(config_.storePath);
    const fs::path parent = storePath.parent_path();
    const fs::path temporaryPath = parent / (storePath.filename().string() + ".tmp." + std::to_string(getpid()));
    unlink(temporaryPath.c_str());

    ScopedFd temporary(open(temporaryPath.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    struct stat temporaryStatus {};
    if (!temporary.valid() || fstat(temporary.get(), &temporaryStatus) != 0 ||
        ((temporaryStatus.st_uid != config_.ownerUid || temporaryStatus.st_gid != config_.ownerGid) &&
         fchown(temporary.get(), config_.ownerUid, config_.ownerGid) != 0) ||
        fchmod(temporary.get(), 0600) != 0) {
        error = "could not create a secure bundle approval transaction";
        unlink(temporaryPath.c_str());
        return false;
    }

    std::ostringstream output;
    output << kStoreHeader << '\n';
    for (const BundleApproval& approval : approvals_) {
        output << approval.userUid << ' ' << approval.appId << ' '
               << hexEncodeDigest(approval.bundleRecordDigest) << ' '
               << publisherStateText(approval.publisherState) << '\n';
    }
    if (!writeAll(temporary.get(), output.str()) || fsync(temporary.get()) != 0 ||
        rename(temporaryPath.c_str(), storePath.c_str()) != 0) {
        error = std::string("could not persist bundle approval transaction: ") + std::strerror(errno);
        unlink(temporaryPath.c_str());
        return false;
    }
    ScopedFd parentDescriptor(open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!parentDescriptor.valid() || fsync(parentDescriptor.get()) != 0) {
        error = "bundle approval transaction committed but parent directory could not be synced";
        return false;
    }
    return true;
}

bool BundleApprovalStore::approve(uid_t userUid, const BundleRecord& record,
                                  BundlePublisherState publisherState, std::string& error) {
    error.clear();
    if (userUid == 0 || publisherState != BundlePublisherState::Unverified ||
        !validateBundleRecord(record, error) || !validateConfig(error) || !validateStoreParent(error)) {
        if (error.empty()) {
            error = "only an unverified bundle may receive a user approval";
        }
        return false;
    }
    ScopedFd lock = openStoreLock(config_, LOCK_EX, error);
    if (!lock.valid() || !loadUnlocked(error)) {
        return false;
    }
    const BundleApproval approval{userUid, record.appId, record.digest, publisherState};
    if (std::any_of(approvals_.begin(), approvals_.end(),
                    [&approval](const BundleApproval& existing) { return sameApproval(existing, approval); })) {
        return true;
    }
    approvals_.push_back(approval);
    if (!persistUnlocked(error)) {
        approvals_.pop_back();
        return false;
    }
    return true;
}

bool BundleApprovalStore::revoke(uid_t userUid, const BundleRecord& record,
                                 BundlePublisherState publisherState, std::string& error) {
    error.clear();
    if (userUid == 0 || publisherState != BundlePublisherState::Unverified ||
        !validateBundleRecord(record, error) || !validateConfig(error) || !validateStoreParent(error)) {
        if (error.empty()) {
            error = "only an unverified bundle can have a user approval revoked";
        }
        return false;
    }
    ScopedFd lock = openStoreLock(config_, LOCK_EX, error);
    if (!lock.valid() || !loadUnlocked(error)) {
        return false;
    }
    const BundleApproval expected{userUid, record.appId, record.digest, publisherState};
    const auto newEnd = std::remove_if(
        approvals_.begin(), approvals_.end(),
        [&expected](const BundleApproval& approval) { return sameApproval(approval, expected); });
    if (newEnd == approvals_.end()) {
        return true;
    }
    approvals_.erase(newEnd, approvals_.end());
    return persistUnlocked(error);
}

bool BundleApprovalStore::isApproved(uid_t userUid, const BundleRecord& record,
                                     BundlePublisherState publisherState, std::string& error) {
    error.clear();
    if (userUid == 0 || publisherState != BundlePublisherState::Unverified ||
        !validateBundleRecord(record, error) || !validateConfig(error) || !validateStoreParent(error)) {
        if (error.empty()) {
            error = "only an unverified bundle can use a user approval";
        }
        return false;
    }
    ScopedFd lock = openStoreLock(config_, LOCK_SH, error);
    if (!lock.valid() || !loadUnlocked(error)) {
        return false;
    }
    const BundleApproval expected{userUid, record.appId, record.digest, publisherState};
    return std::any_of(approvals_.begin(), approvals_.end(),
                       [&expected](const BundleApproval& approval) { return sameApproval(approval, expected); });
}

} // namespace lcl::security
