#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "system/security/sandbox_child_launcher.hpp"

namespace lcl::security {

/**
 * Trusted verifier-side launch descriptors.  They are never serialized or
 * accepted from the app/session launch protocol: sandboxd duplicates and owns
 * each descriptor before retaining this material.
 */
struct SandboxLaunchMaterialInput {
    VerifiedApplication application;
    SandboxFilesystemSources filesystemSources;
    /** Exact, verified path of executableDescriptor below the app bundle root. */
    std::string executableBundlePath;
    int executableDescriptor{-1};
    int runtimeDescriptor{-1};
    std::vector<std::string> arguments;
};

/** Root daemon registry that retains the descriptor snapshot for an approved bundle. */
class SandboxLaunchMaterialRegistry final {
public:
    SandboxLaunchMaterialRegistry();
    ~SandboxLaunchMaterialRegistry();

    SandboxLaunchMaterialRegistry(const SandboxLaunchMaterialRegistry&) = delete;
    SandboxLaunchMaterialRegistry& operator=(const SandboxLaunchMaterialRegistry&) = delete;

    bool registerMaterial(const SandboxLaunchMaterialInput& input, std::string& error);
    void remove(const std::string& appId);
    std::size_t size() const noexcept { return materials_.size(); }

    /**
     * Checks whether a verified descriptor snapshot can satisfy this exact
     * policy-bound request.  It performs no allocation and exposes no
     * descriptor, so sandboxd can reject a stale request before it creates a
     * cgroup or forks a child.
     */
    bool hasMaterialFor(const SandboxLaunchPlan& plan) const;

    /** Rebuilds protected child launch data for an already-authorized plan. */
    bool makeChildLaunchSpec(const SandboxLaunchPlan& plan,
                             const SandboxPlatformHardening& hardening,
                             std::optional<SandboxCgroupBinding> cgroup,
                             SandboxChildLaunchSpec& spec,
                             std::string& error) const;

private:
    struct StoredMaterial;

    std::unordered_map<std::string, std::unique_ptr<StoredMaterial>> materials_;
};

} // namespace lcl::security
