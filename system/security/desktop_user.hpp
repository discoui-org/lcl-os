#pragma once

#include <string>
#include <sys/types.h>

namespace lcl::security {

/** Canonical unprivileged desktop identity in the LCL rootfs. */
inline constexpr uid_t kDesktopUserUid = 1000;
inline constexpr gid_t kDesktopUserGid = 1000;
inline constexpr const char* kDesktopUserName = "Rei";
inline constexpr const char* kDesktopUserHome = "/Users/Rei";

/**
 * Drops a root-owned child to the desktop identity, clears all capabilities
 * and forbids future privilege gains. A non-root caller remains unprivileged.
 */
bool dropToDesktopUser(std::string& error);

/** Root-only boot/session provisioning for the writable desktop home. */
bool provisionDesktopUserHome(std::string& error);

/** Assigns a root-created runtime socket or file to the desktop identity. */
bool assignDesktopUserOwnership(const std::string& path, mode_t mode, std::string& error);

} // namespace lcl::security
