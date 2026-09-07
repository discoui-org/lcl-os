#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace lcl::security {

/** Canonical unprivileged interactive session identity in the LCL rootfs. */
inline constexpr uid_t kSessionUserUid = 1000;
inline constexpr gid_t kSessionUserGid = 1000;
/** Supplementary group used only for compositor/raster client endpoints. */
inline constexpr gid_t kApplicationRuntimeGid = 62000;
inline constexpr const char* kSessionUserName = "Rei";
inline constexpr const char* kSessionUserHome = "/Users/Rei";

/**
 * Terminal is a deliberately trusted interactive shell, not a third-party
 * sandbox profile.  The profile is bound to the immutable system bundle so a
 * user bundle cannot acquire it merely by reusing Terminal's app identifier.
 */
inline constexpr const char* kTrustedUserShellAppId = "org.lcl.terminal";
inline constexpr const char* kTrustedUserShellBundlePath =
    "/System/Applications/Terminal.app";
inline constexpr const char* kTrustedUserShellProfileId =
    "lcl.trusted-user-shell.v1";

/** Immutable Settings is the sole recipient of the security admin capability. */
inline constexpr const char* kSystemSettingsAppId = "org.lcl.settings";
inline constexpr const char* kSystemSettingsBundlePath =
    "/System/Applications/Settings.app";
inline constexpr const char* kSystemSettingsProfileId = "lcl.system-settings.v1";

/**
 * Drops a root-owned child to the interactive session identity, clears all capabilities
 * and forbids future privilege gains. A non-root caller remains unprivileged.
 */
bool dropToSessionUser(std::string& error);

/** Returns true only for the canonical, rootfs-provided Terminal.app bundle. */
bool isTrustedUserShellBundle(std::string_view appId, std::string_view bundlePath) noexcept;
bool isSystemSettingsBundle(std::string_view appId, std::string_view bundlePath) noexcept;

/**
 * Replaces inherited init/session environment with the minimal environment of
 * the trusted user shell.  This must run before dropping credentials and exec.
 */
bool prepareTrustedUserShellEnvironment(uint64_t instanceId, std::string& error);
/** Minimal unprivileged environment for the canonical Settings system app. */
bool prepareSystemSettingsEnvironment(uint64_t instanceId, std::string& error);

/** Root-only boot/session provisioning for the writable session home. */
bool provisionSessionUserHome(std::string& error);

/** Assigns a root-created runtime socket or file to the session identity. */
bool assignSessionUserOwnership(const std::string& path, mode_t mode, std::string& error);

/** Makes a root-created graphics endpoint reachable by the session and sandbox app group. */
bool assignApplicationRuntimeOwnership(const std::string& path, std::string& error);

} // namespace lcl::security
