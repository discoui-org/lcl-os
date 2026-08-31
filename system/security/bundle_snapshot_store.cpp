#include "system/security/bundle_snapshot_store.hpp"

#include "system/security/sandbox_filesystem_sources.hpp"
#include "system/session/app_bundle_parser.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace lcl::security {
namespace {

namespace fs = std::filesystem;

constexpr std::uint64_t kMaximumBundleFileBytes = 128ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumSignatureEnvelopeBytes = 64ULL * 1024ULL;
constexpr std::size_t kCopyBufferBytes = 64U * 1024U;

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
    ScopedFd(ScopedFd&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {}
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

bool sameInode(const struct stat& left, const struct stat& right) {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

bool isPrivateOwnedDirectory(const struct stat& status, uid_t ownerUid, gid_t ownerGid) {
    return S_ISDIR(status.st_mode) && status.st_uid == ownerUid &&
           status.st_gid == ownerGid && (status.st_mode & 0077) == 0;
}

bool isRootControlledDirectory(const struct stat& status, uid_t ownerUid, gid_t ownerGid) {
    return S_ISDIR(status.st_mode) && status.st_uid == ownerUid &&
           status.st_gid == ownerGid && (status.st_mode & 0022) == 0;
}

bool ensureSnapshotsRoot(const BundleSnapshotStoreConfig& config, ScopedFd& root,
                         std::string& error) {
    const fs::path rootPath(config.snapshotsRoot);
    const fs::path parent = rootPath.parent_path();
    struct stat parentStatus {};
    if (config.snapshotsRoot.empty() || !rootPath.is_absolute() || parent.empty() ||
        lstat(parent.c_str(), &parentStatus) != 0 ||
        !isPrivateOwnedDirectory(parentStatus, config.ownerUid, config.ownerGid)) {
        error = "bundle snapshot parent is not a private owner-controlled directory";
        return false;
    }
    if (mkdir(config.snapshotsRoot.c_str(), 0700) != 0 && errno != EEXIST) {
        error = std::string("could not create bundle snapshot store: ") + std::strerror(errno);
        return false;
    }
    ScopedFd opened(open(config.snapshotsRoot.c_str(),
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat status {};
    if (!opened.valid() || fstat(opened.get(), &status) != 0 ||
        !isPrivateOwnedDirectory(status, config.ownerUid, config.ownerGid) ||
        fchmod(opened.get(), 0700) != 0) {
        error = "bundle snapshot store is not a private owner-controlled directory";
        return false;
    }
    root = std::move(opened);
    return true;
}

bool splitRelativePath(std::string_view relativePath,
                       std::vector<std::string_view>& components) {
    components.clear();
    if (!isSafeSandboxBundleRelativePath(std::string(relativePath))) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < relativePath.size()) {
        const std::size_t separator = relativePath.find('/', offset);
        components.push_back(relativePath.substr(offset, separator - offset));
        if (separator == std::string_view::npos) {
            return true;
        }
        offset = separator + 1;
    }
    return false;
}

ScopedFd openSourceFile(int bundleDescriptor, std::string_view relativePath,
                        std::string& error) {
    std::vector<std::string_view> components;
    if (!splitRelativePath(relativePath, components)) {
        error = "bundle snapshot received an unsafe relative file path";
        return {};
    }
    ScopedFd current(fcntl(bundleDescriptor, F_DUPFD_CLOEXEC, 8));
    if (!current.valid()) {
        error = std::string("could not duplicate verified bundle descriptor: ") +
                std::strerror(errno);
        return {};
    }
    for (std::size_t index = 0; index < components.size(); ++index) {
        const bool finalComponent = index + 1 == components.size();
        const std::string name(components[index]);
        ScopedFd opened(openat(current.get(), name.c_str(),
                               O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
                                   (finalComponent ? 0 : O_DIRECTORY)));
        if (!opened.valid()) {
            error = std::string("could not reopen verified bundle payload: ") +
                    std::strerror(errno);
            return {};
        }
        current = std::move(opened);
    }
    return current;
}

ScopedFd ensureDestinationParent(int snapshotDescriptor,
                                 const std::vector<std::string_view>& components,
                                 const BundleSnapshotStoreConfig& config,
                                 std::string& error) {
    ScopedFd current(fcntl(snapshotDescriptor, F_DUPFD_CLOEXEC, 8));
    if (!current.valid()) {
        error = std::string("could not duplicate bundle snapshot descriptor: ") +
                std::strerror(errno);
        return {};
    }
    for (std::size_t index = 0; index + 1 < components.size(); ++index) {
        const std::string name(components[index]);
        if (mkdirat(current.get(), name.c_str(), 0755) != 0 && errno != EEXIST) {
            error = std::string("could not create bundle snapshot directory: ") +
                    std::strerror(errno);
            return {};
        }
        ScopedFd opened(openat(current.get(), name.c_str(),
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        struct stat status {};
        if (!opened.valid() || fstat(opened.get(), &status) != 0 ||
            !isRootControlledDirectory(status, config.ownerUid, config.ownerGid) ||
            fchmod(opened.get(), 0755) != 0) {
            error = "bundle snapshot directory is not protected";
            return {};
        }
        current = std::move(opened);
    }
    return current;
}

bool writeAll(int descriptor, const std::array<char, kCopyBufferBytes>& buffer,
              std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t count = write(descriptor, buffer.data() + offset, size - offset);
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

bool copyOneFile(int bundleDescriptor, int snapshotDescriptor,
                 const BundleFileDigest& expected,
                 const BundleSnapshotStoreConfig& config,
                 std::string& error) {
    std::vector<std::string_view> components;
    if (!splitRelativePath(expected.relativePath, components) || components.empty()) {
        error = "bundle snapshot received an unsafe record path";
        return false;
    }
    ScopedFd source = openSourceFile(bundleDescriptor, expected.relativePath, error);
    struct stat sourceStatus {};
    if (!source.valid() || fstat(source.get(), &sourceStatus) != 0 ||
        !S_ISREG(sourceStatus.st_mode) || sourceStatus.st_nlink != 1 ||
        (sourceStatus.st_mode & 0777) != expected.mode || sourceStatus.st_size < 0 ||
        static_cast<std::uint64_t>(sourceStatus.st_size) > kMaximumBundleFileBytes) {
        if (error.empty()) {
            error = "bundle payload changed before snapshot staging";
        }
        return false;
    }
    ScopedFd destinationParent =
        ensureDestinationParent(snapshotDescriptor, components, config, error);
    if (!destinationParent.valid()) {
        return false;
    }
    const std::string fileName(components.back());
    ScopedFd destination(openat(destinationParent.get(), fileName.c_str(),
                                O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                                0600));
    if (!destination.valid()) {
        error = std::string("could not create bundle snapshot payload: ") + std::strerror(errno);
        return false;
    }

    std::array<char, kCopyBufferBytes> buffer {};
    std::uint64_t offset = 0;
    while (offset < static_cast<std::uint64_t>(sourceStatus.st_size)) {
        const std::size_t requested = static_cast<std::size_t>(std::min<std::uint64_t>(
            buffer.size(), static_cast<std::uint64_t>(sourceStatus.st_size) - offset));
        const ssize_t count = pread(source.get(), buffer.data(), requested,
                                    static_cast<off_t>(offset));
        if (count > 0) {
            if (!writeAll(destination.get(), buffer, static_cast<std::size_t>(count))) {
                error = std::string("could not write bundle snapshot payload: ") +
                        std::strerror(errno);
                return false;
            }
            offset += static_cast<std::uint64_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        error = "could not read verified bundle payload during snapshot staging";
        return false;
    }
    struct stat finalSourceStatus {};
    if (fstat(source.get(), &finalSourceStatus) != 0 ||
        !sameInode(sourceStatus, finalSourceStatus) ||
        finalSourceStatus.st_size != sourceStatus.st_size ||
        finalSourceStatus.st_nlink != 1 || fsync(destination.get()) != 0 ||
        sha256FileDescriptor(destination.get(), kMaximumBundleFileBytes) !=
            std::optional<Sha256Digest>(expected.digest) ||
        fchown(destination.get(), config.ownerUid, config.ownerGid) != 0 ||
        fchmod(destination.get(), static_cast<mode_t>(expected.mode)) != 0) {
        error = "bundle payload changed or could not be secured during snapshot staging";
        return false;
    }
    return true;
}

bool copySignatureEnvelopeIfPresent(int bundleDescriptor, int snapshotDescriptor,
                                    const BundleRecord& record,
                                    const BundleSnapshotStoreConfig& config,
                                    std::string& error) {
    if (isZeroDigest(record.signatureEnvelopeDigest)) {
        return true;
    }
    ScopedFd source = openSourceFile(bundleDescriptor, "Signature.ed25519", error);
    struct stat status {};
    const auto digest = source.valid()
        ? sha256FileDescriptor(source.get(), kMaximumSignatureEnvelopeBytes)
        : std::nullopt;
    if (!source.valid() || fstat(source.get(), &status) != 0 ||
        !S_ISREG(status.st_mode) || status.st_nlink != 1 || status.st_size < 0 ||
        static_cast<std::uint64_t>(status.st_size) > kMaximumSignatureEnvelopeBytes || !digest ||
        *digest != record.signatureEnvelopeDigest) {
        if (error.empty()) {
            error = "bundle signature envelope changed before snapshot staging";
        }
        return false;
    }
    const BundleFileDigest envelope{
        .relativePath = "Signature.ed25519",
        .mode = static_cast<std::uint16_t>(status.st_mode & 0777),
        .digest = *digest,
    };
    return copyOneFile(bundleDescriptor, snapshotDescriptor, envelope, config, error);
}

bool verifyExistingSnapshot(const fs::path& path, const BundleRecord& expected,
                            lcl::core::AppBundleMetadata& snapshot, std::string& error) {
    const auto parsed = lcl::core::AppBundleParser::parseBundle(path.string());
    if (!parsed || !parsed->valid) {
        error = "existing protected bundle snapshot cannot be parsed";
        return false;
    }
    const auto actual = makeBundleRecord(*parsed, error);
    if (!actual || actual->digest != expected.digest || actual->appId != expected.appId) {
        if (error.empty()) {
            error = "existing protected bundle snapshot does not match its record";
        }
        return false;
    }
    snapshot = *parsed;
    return true;
}

} // namespace

BundleSnapshotStore::BundleSnapshotStore(BundleSnapshotStoreConfig config)
    : config_(std::move(config)) {}

bool BundleSnapshotStore::stage(const lcl::core::AppBundleMetadata& source,
                                const BundleRecord& record,
                                lcl::core::AppBundleMetadata& snapshot,
                                std::string& error) const {
    error.clear();
    snapshot = {};
    if (!source.valid || !source.bundleHandle || !source.bundleHandle->valid() ||
        !validateBundleRecord(record, error) || source.appId != record.appId) {
        if (error.empty()) {
            error = "bundle snapshot source does not match its verified record";
        }
        return false;
    }

    ScopedFd root;
    if (!ensureSnapshotsRoot(config_, root, error)) {
        return false;
    }
    const std::string snapshotName = hexEncodeDigest(record.digest) + ".app";
    struct stat existingStatus {};
    if (fstatat(root.get(), snapshotName.c_str(), &existingStatus, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!isRootControlledDirectory(existingStatus, config_.ownerUid, config_.ownerGid) ||
            (existingStatus.st_mode & 0022) != 0) {
            error = "existing bundle snapshot is not protected";
            return false;
        }
        return verifyExistingSnapshot(fs::path(config_.snapshotsRoot) / snapshotName,
                                      record, snapshot, error);
    }
    if (errno != ENOENT) {
        error = std::string("could not inspect bundle snapshot path: ") + std::strerror(errno);
        return false;
    }

    std::string temporaryName;
    ScopedFd temporary;
    for (unsigned int attempt = 0; attempt != 128; ++attempt) {
        temporaryName = ".stage-" + std::to_string(getpid()) + "-" + std::to_string(attempt);
        if (mkdirat(root.get(), temporaryName.c_str(), 0700) == 0) {
            temporary = ScopedFd(openat(root.get(), temporaryName.c_str(),
                                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
            break;
        }
        if (errno != EEXIST) {
            error = std::string("could not create bundle snapshot transaction: ") +
                    std::strerror(errno);
            return false;
        }
    }
    if (!temporary.valid()) {
        error = "could not allocate a bundle snapshot transaction name";
        return false;
    }
    const auto cleanup = [&] {
        std::error_code cleanupError;
        fs::remove_all(fs::path(config_.snapshotsRoot) / temporaryName, cleanupError);
    };

    struct stat temporaryStatus {};
    if (fstat(temporary.get(), &temporaryStatus) != 0 ||
        !isPrivateOwnedDirectory(temporaryStatus, config_.ownerUid, config_.ownerGid)) {
        error = "bundle snapshot transaction directory is not protected";
        cleanup();
        return false;
    }
    for (const BundleFileDigest& file : record.files) {
        if (!copyOneFile(source.bundleHandle->descriptor(), temporary.get(), file, config_, error)) {
            cleanup();
            return false;
        }
    }
    if (!copySignatureEnvelopeIfPresent(source.bundleHandle->descriptor(), temporary.get(), record,
                                       config_, error)) {
        cleanup();
        return false;
    }
    if (fchmod(temporary.get(), 0755) != 0 || fsync(temporary.get()) != 0 ||
        renameat(root.get(), temporaryName.c_str(), root.get(), snapshotName.c_str()) != 0 ||
        fsync(root.get()) != 0) {
        error = std::string("could not commit protected bundle snapshot: ") + std::strerror(errno);
        cleanup();
        return false;
    }
    return verifyExistingSnapshot(fs::path(config_.snapshotsRoot) / snapshotName,
                                  record, snapshot, error);
}

} // namespace lcl::security
