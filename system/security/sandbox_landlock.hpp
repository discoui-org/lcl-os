#pragma once

#include <string>

#include "system/security/app_identity_registry.hpp"
#include "system/security/sandbox_filesystem_sources.hpp"

namespace lcl::security {

/**
 * Protected descriptor set for the Landlock layer of a prepared app mount
 * namespace.  It deliberately contains neither host paths nor application
 * input: sandboxd opens and validates every directory before entering the
 * child namespace.
 */
struct SandboxLandlockRules {
    AppIdentity identity;
    SandboxFilesystemSources filesystemSources;
    /** The exact executable/script descriptor retained for execveat. */
    int executableDescriptor{-1};
    int temporaryDescriptor{-1};
    int deviceDescriptor{-1};
    /** Read-only sysfs view paired with graphics.render-node, if present. */
    int sysfsDescriptor{-1};
};

/** Verifies ownership and descriptor shape before a Landlock ruleset is built. */
bool validateSandboxLandlockRules(const SandboxLandlockRules& rules, std::string& error);

/**
 * Installs the default-deny filesystem Landlock ruleset in the current child.
 * The caller must already have set PR_SET_NO_NEW_PRIVS and entered its private
 * mount namespace.  Unsupported or insufficient Landlock ABIs fail closed.
 */
bool installSandboxLandlockRules(const SandboxLandlockRules& rules, std::string& error);

} // namespace lcl::security
