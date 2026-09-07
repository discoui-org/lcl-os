#pragma once

#include <string>

#include "system/security/app_identity_registry.hpp"

namespace lcl::security {

/**
 * Descriptor-only sources for a future private app mount namespace.
 *
 * These are protected sandboxd inputs, acquired during verified bundle and
 * app-container resolution. They are intentionally not serializable and no
 * application IPC may supply a pathname for any of them.
 */
struct SandboxFilesystemSources {
    int appBundleDescriptor{-1};
    int systemDescriptor{-1};
    int dataDescriptor{-1};
    int cacheDescriptor{-1};
    int preferencesDescriptor{-1};
    int compositorSocketDescriptor{-1};
    int rasterSocketDescriptor{-1};
};

/**
 * Checks descriptor type and ownership before sandboxd uses a source as a
 * bind-mount root. Persistent app storage must already be exactly app-owned
 * and 0700; system data must be root-owned and non-writable by group/other.
 */
bool validateSandboxFilesystemSources(const SandboxFilesystemSources& sources,
                                     const AppIdentity& identity, std::string& error);

/**
 * Verifies that the retained graphics endpoint descriptors still name the
 * linked socket objects at their original absolute paths.  A compositor or
 * rasterd restart unlinks and recreates its socket; keeping the old
 * descriptor would otherwise let a private sandbox bind mount retain a
 * stale endpoint generation.
 */
bool validateSandboxRuntimeEndpointGeneration(const SandboxFilesystemSources& sources,
                                              std::string& error);

/** Validates an executable path relative to the protected app-bundle root. */
bool isSafeSandboxBundleRelativePath(const std::string& path) noexcept;

} // namespace lcl::security
