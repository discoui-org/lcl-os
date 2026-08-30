#pragma once

#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace lcl::core {

/**
 * A read-only descriptor retained by the trusted session catalog.
 *
 * It pins a verified inode across a later rename or symlink swap. The handle
 * is shared by metadata copies and is not exposed through the app protocol.
 */
class AppBundleFileHandle final {
public:
    explicit AppBundleFileHandle(int descriptor) noexcept;
    ~AppBundleFileHandle();

    AppBundleFileHandle(const AppBundleFileHandle&) = delete;
    AppBundleFileHandle& operator=(const AppBundleFileHandle&) = delete;

    int descriptor() const noexcept { return m_descriptor; }
    bool valid() const noexcept { return m_descriptor >= 0; }

private:
    int m_descriptor{-1};
};

struct AppBundleMetadata {
    std::string bundlePath;      // e.g. "/System/Applications/Terminal.app"
    std::string appId;           // Canonical manifest identity, e.g. "org.lcl.terminal"
    std::string name;            // e.g. "Terminal"
    std::string executablePath;  // e.g. "/System/Applications/Terminal.app/Executables/Terminal"
    std::string version;         // e.g. "1.0.0"
    std::string icon;            // e.g. "Resources/Icon.png"
    std::string executable;      // e.g. "Executables/Terminal", bundle-relative
    // Exact bytes parsed from Manifest.json; retained only in the trusted
    // catalog so a later verifier can detect an in-place manifest rewrite.
    std::string manifestContents;
    std::string type;            // "gui" or "cli"
    std::string runtime;         // e.g. "org.lcl.javascript"
    // Declarations only. They grant nothing until a permission broker evaluates them.
    std::vector<std::string> requestedPermissions;
    // These descriptors pin the exact bundle snapshot that was schema-checked.
    // Security services use them to reject a bundle changed between cataloging,
    // verification, approval and execution.
    std::shared_ptr<const AppBundleFileHandle> bundleHandle;
    std::shared_ptr<const AppBundleFileHandle> manifestHandle;
    // Optional detached signature envelope at bundle root. Its descriptor is
    // retained so verification never reopens a user-controlled pathname.
    std::shared_ptr<const AppBundleFileHandle> signatureHandle;
    std::shared_ptr<const AppBundleFileHandle> iconHandle;
    std::shared_ptr<const AppBundleFileHandle> executableHandle;
    bool valid{false};
};

class AppBundleParser {
public:
    /**
     * @brief Parse a strict .app bundle containing Manifest.json and Resources/.
     * @param bundlePath Absolute or relative path to the .app directory
     * @return AppBundleMetadata if valid, std::nullopt otherwise
     */
    static std::optional<AppBundleMetadata> parseBundle(const std::string& bundlePath);

    /**
     * @brief Scan a directory for all .app bundles.
     * @param searchDir Directory to scan (e.g. "/home/user/Applications")
     * @return List of parsed AppBundleMetadata objects
     */
    static std::vector<AppBundleMetadata> scanDirectory(const std::string& searchDir);
};

} // namespace lcl::core
