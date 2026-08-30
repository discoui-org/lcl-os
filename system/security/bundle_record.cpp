#include "system/security/bundle_record.hpp"

#include "system/security/app_identity_registry.hpp"
#include "system/session/app_bundle_parser.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace lcl::security {
namespace {

constexpr std::size_t kMaximumBundleFiles = 4096;
constexpr std::uint64_t kMaximumBundleFileBytes = 128ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumBundleBytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumRelativePathBytes = 1024;
constexpr std::uint32_t kMaximumBundleDirectoryDepth = 32;
constexpr std::string_view kSignatureEnvelopeName = "Signature.ed25519";

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

class ScopedDir final {
public:
    explicit ScopedDir(DIR* directory = nullptr) noexcept : directory_(directory) {}
    ~ScopedDir() {
        if (directory_) {
            closedir(directory_);
        }
    }

    ScopedDir(const ScopedDir&) = delete;
    ScopedDir& operator=(const ScopedDir&) = delete;
    DIR* get() const noexcept { return directory_; }

private:
    DIR* directory_{nullptr};
};

void appendU16(std::string& output, std::uint16_t value) {
    output.push_back(static_cast<char>(value >> 8));
    output.push_back(static_cast<char>(value));
}

void appendU32(std::string& output, std::uint32_t value) {
    for (int index = 3; index >= 0; --index) {
        output.push_back(static_cast<char>(value >> (index * 8)));
    }
}

void appendString(std::string& output, std::string_view value) {
    appendU32(output, static_cast<std::uint32_t>(value.size()));
    output.append(value);
}

void appendDigest(std::string& output, const Sha256Digest& digest) {
    output.append(reinterpret_cast<const char*>(digest.data()), digest.size());
}

bool hasSafeText(const std::string& value, std::size_t maximumLength, bool allowEmpty = false) {
    if ((value.empty() && !allowEmpty) || value.size() > maximumLength ||
        value.find('\0') != std::string::npos) {
        return false;
    }
    for (const unsigned char byte : value) {
        if (byte < 0x20 || byte == 0x7F) {
            return false;
        }
    }
    return true;
}

bool isSafeRelativePath(const std::string& path) {
    if (!hasSafeText(path, kMaximumRelativePathBytes) || path.front() == '/' ||
        path.find('\\') != std::string::npos || path.find("//") != std::string::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start < path.size()) {
        const std::size_t end = path.find('/', start);
        const std::string_view component(path.data() + start, end - start);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return true;
}

bool sameInode(const struct stat& expected, const struct stat& actual) {
    return expected.st_dev == actual.st_dev && expected.st_ino == actual.st_ino;
}

bool isSafeRegularFile(const struct stat& status) {
    return S_ISREG(status.st_mode) && status.st_nlink == 1 && status.st_size >= 0 &&
           static_cast<std::uint64_t>(status.st_size) <= kMaximumBundleFileBytes;
}

bool scanDirectory(int directoryDescriptor, std::string_view prefix,
                   std::vector<BundleFileDigest>& files, std::uint64_t& totalBytes,
                   std::size_t& entryCount, std::uint32_t depth, std::string& error) {
    if (depth > kMaximumBundleDirectoryDepth) {
        error = "bundle directory nesting exceeds verifier limit";
        return false;
    }
    ScopedFd readDescriptor(openat(directoryDescriptor, ".",
                                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!readDescriptor.valid()) {
        error = "could not reopen bundle directory by descriptor";
        return false;
    }
    const int enumerationDescriptor = readDescriptor.release();
    ScopedDir directory(fdopendir(enumerationDescriptor));
    if (!directory.get()) {
        close(enumerationDescriptor);
        error = "could not enumerate bundle directory";
        return false;
    }

    std::vector<std::string> names;
    errno = 0;
    while (dirent* entry = readdir(directory.get())) {
        const std::string name(entry->d_name);
        if (name == "." || name == "..") {
            continue;
        }
        if (!hasSafeText(name, kMaximumRelativePathBytes) || name.find('/') != std::string::npos ||
            name.find('\\') != std::string::npos) {
            error = "bundle contains an unsafe directory entry name";
            return false;
        }
        if (entryCount + names.size() >= kMaximumBundleFiles) {
            error = "bundle exceeds verifier entry limit";
            return false;
        }
        names.push_back(name);
    }
    if (errno != 0) {
        error = "could not finish enumerating bundle directory";
        return false;
    }
    std::sort(names.begin(), names.end());

    for (const std::string& name : names) {
        if (entryCount >= kMaximumBundleFiles) {
            error = "bundle exceeds verifier entry limit";
            return false;
        }
        ++entryCount;
        struct stat entryStatus {};
        if (fstatat(directoryDescriptor, name.c_str(), &entryStatus, AT_SYMLINK_NOFOLLOW) != 0) {
            error = "bundle changed while enumerating files";
            return false;
        }
        const std::string relativePath = prefix.empty() ? name : std::string(prefix) + "/" + name;
        if (S_ISLNK(entryStatus.st_mode)) {
            error = "bundle contains a symbolic link";
            return false;
        }
        if (S_ISDIR(entryStatus.st_mode)) {
            ScopedFd child(openat(directoryDescriptor, name.c_str(),
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
            struct stat openedStatus {};
            if (!child.valid() || fstat(child.get(), &openedStatus) != 0 || !S_ISDIR(openedStatus.st_mode) ||
                !sameInode(entryStatus, openedStatus) ||
                !scanDirectory(child.get(), relativePath, files, totalBytes, entryCount, depth + 1, error)) {
                if (error.empty()) {
                    error = "bundle directory changed while opening";
                }
                return false;
            }
            continue;
        }
        if (!isSafeRegularFile(entryStatus)) {
            error = "bundle contains a non-regular file or hard link";
            return false;
        }
        // A detached signature cannot sign an encoding that contains its own
        // bytes. It is therefore checked as a safe envelope but deliberately
        // excluded from the signed BundleRecord payload.
        if (prefix.empty() && name == kSignatureEnvelopeName) {
            ScopedFd signature(openat(directoryDescriptor, name.c_str(),
                                      O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
            struct stat openedStatus {};
            if (!signature.valid() || fstat(signature.get(), &openedStatus) != 0 ||
                !isSafeRegularFile(openedStatus) || !sameInode(entryStatus, openedStatus)) {
                error = "bundle signature envelope changed while opening";
                return false;
            }
            continue;
        }
        const std::uint64_t byteSize = static_cast<std::uint64_t>(entryStatus.st_size);
        if (files.size() >= kMaximumBundleFiles || byteSize > kMaximumBundleBytes - totalBytes) {
            error = "bundle exceeds verifier file or byte limit";
            return false;
        }

        ScopedFd file(openat(directoryDescriptor, name.c_str(),
                             O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        struct stat openedStatus {};
        if (!file.valid() || fstat(file.get(), &openedStatus) != 0 ||
            !isSafeRegularFile(openedStatus) || !sameInode(entryStatus, openedStatus)) {
            error = "bundle file changed while opening";
            return false;
        }
        const auto digest = sha256FileDescriptor(file.get(), kMaximumBundleFileBytes);
        if (!digest) {
            error = "could not hash a bundle file";
            return false;
        }
        files.push_back(BundleFileDigest{relativePath,
                                         static_cast<std::uint16_t>(openedStatus.st_mode & 0777),
                                         *digest});
        totalBytes += byteSize;
    }
    return true;
}

bool retainedFileMatchesRecord(const std::shared_ptr<const lcl::core::AppBundleFileHandle>& handle,
                               std::string_view relativePath,
                               const std::vector<BundleFileDigest>& files,
                               const std::optional<Sha256Digest>& expectedDigest,
                               std::string& error) {
    if (!handle || !handle->valid()) {
        error = "bundle parser did not retain a verified file descriptor";
        return false;
    }
    const auto found = std::lower_bound(
        files.begin(), files.end(), relativePath,
        [](const BundleFileDigest& file, std::string_view path) { return file.relativePath < path; });
    if (found == files.end() || found->relativePath != relativePath) {
        error = "bundle record is missing a required manifest file";
        return false;
    }

    struct stat status {};
    const auto digest = sha256FileDescriptor(handle->descriptor(), kMaximumBundleFileBytes);
    if (!digest || fstat(handle->descriptor(), &status) != 0 || !isSafeRegularFile(status) ||
        found->mode != static_cast<std::uint16_t>(status.st_mode & 0777) || found->digest != *digest) {
        error = "bundle changed after manifest schema validation";
        return false;
    }
    if (expectedDigest && *digest != *expectedDigest) {
        error = "manifest changed after schema validation";
        return false;
    }
    return true;
}

} // namespace

Sha256Digest digestBundleRecord(const BundleRecord& record) {
    std::string canonical;
    canonical.reserve(128 + record.appId.size() + record.appVersion.size() + record.type.size() +
                      record.runtime.size() + record.requestedPermissions.size() * 32 +
                      record.files.size() * 96);
    canonical.append("LCL_BUNDLE_RECORD_V1");
    appendU32(canonical, record.recordVersion);
    appendString(canonical, record.appId);
    appendString(canonical, record.appVersion);
    appendString(canonical, record.type);
    appendString(canonical, record.runtime);
    appendU32(canonical, static_cast<std::uint32_t>(record.requestedPermissions.size()));
    for (const std::string& permission : record.requestedPermissions) {
        appendString(canonical, permission);
    }
    appendU32(canonical, static_cast<std::uint32_t>(record.files.size()));
    for (const BundleFileDigest& file : record.files) {
        appendString(canonical, file.relativePath);
        appendU16(canonical, file.mode);
        appendDigest(canonical, file.digest);
    }
    return sha256(canonical);
}

bool validateBundleRecord(const BundleRecord& record, std::string& error) {
    error.clear();
    if (record.recordVersion != kBundleRecordVersion ||
        !AppIdentityRegistry::isValidAppId(record.appId) ||
        !hasSafeText(record.appVersion, 128, true) || (record.type != "gui" && record.type != "cli") ||
        !hasSafeText(record.runtime, 128, true) || record.files.empty() ||
        record.files.size() > kMaximumBundleFiles || isZeroDigest(record.digest)) {
        error = "bundle record has invalid core fields";
        return false;
    }

    for (std::size_t index = 0; index < record.requestedPermissions.size(); ++index) {
        if (!AppIdentityRegistry::isValidAppId(record.requestedPermissions[index]) ||
            (index > 0 && record.requestedPermissions[index - 1] >= record.requestedPermissions[index])) {
            error = "bundle record permissions are not canonical";
            return false;
        }
    }
    for (std::size_t index = 0; index < record.files.size(); ++index) {
        const BundleFileDigest& file = record.files[index];
        if (!isSafeRelativePath(file.relativePath) || (file.mode & ~0777U) != 0 ||
            isZeroDigest(file.digest) ||
            (index > 0 && record.files[index - 1].relativePath >= file.relativePath)) {
            error = "bundle record files are not canonical";
            return false;
        }
    }
    if (digestBundleRecord(record) != record.digest) {
        error = "bundle record digest does not match canonical contents";
        return false;
    }
    return true;
}

std::optional<BundleRecord> makeBundleRecord(const lcl::core::AppBundleMetadata& metadata,
                                             std::string& error) {
    error.clear();
    if (!metadata.valid || !metadata.bundleHandle || !metadata.bundleHandle->valid() ||
        !AppIdentityRegistry::isValidAppId(metadata.appId) ||
        !hasSafeText(metadata.version, 128, true) ||
        (metadata.type != "gui" && metadata.type != "cli") || !hasSafeText(metadata.runtime, 128, true) ||
        !isSafeRelativePath(metadata.icon) || !isSafeRelativePath(metadata.executable)) {
        error = "bundle metadata is incomplete or unsafe";
        return std::nullopt;
    }

    struct stat bundleStatus {};
    if (fstat(metadata.bundleHandle->descriptor(), &bundleStatus) != 0 || !S_ISDIR(bundleStatus.st_mode)) {
        error = "bundle root descriptor is no longer a directory";
        return std::nullopt;
    }

    BundleRecord record{};
    record.appId = metadata.appId;
    record.appVersion = metadata.version;
    record.type = metadata.type;
    record.runtime = metadata.runtime;
    record.requestedPermissions = metadata.requestedPermissions;
    std::sort(record.requestedPermissions.begin(), record.requestedPermissions.end());
    for (std::size_t index = 0; index < record.requestedPermissions.size(); ++index) {
        if (!AppIdentityRegistry::isValidAppId(record.requestedPermissions[index]) ||
            (index > 0 && record.requestedPermissions[index - 1] == record.requestedPermissions[index])) {
            error = "bundle metadata has invalid requested permissions";
            return std::nullopt;
        }
    }

    std::uint64_t totalBytes = 0;
    std::size_t entryCount = 0;
    if (!scanDirectory(metadata.bundleHandle->descriptor(), "", record.files, totalBytes, entryCount, 0, error)) {
        return std::nullopt;
    }
    std::sort(record.files.begin(), record.files.end(),
              [](const BundleFileDigest& left, const BundleFileDigest& right) {
                  return left.relativePath < right.relativePath;
              });
    if (!retainedFileMatchesRecord(metadata.manifestHandle, "Manifest.json", record.files,
                                   sha256(metadata.manifestContents), error) ||
        !retainedFileMatchesRecord(metadata.iconHandle, metadata.icon, record.files, std::nullopt, error) ||
        !retainedFileMatchesRecord(metadata.executableHandle, metadata.executable, record.files,
                                   std::nullopt, error)) {
        return std::nullopt;
    }

    record.digest = digestBundleRecord(record);
    if (!validateBundleRecord(record, error)) {
        return std::nullopt;
    }
    return record;
}

} // namespace lcl::security
