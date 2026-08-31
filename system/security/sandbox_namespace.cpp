#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "system/security/sandbox_namespace.hpp"

#include <sched.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace lcl::security {

bool beginSandboxNamespaces(const SandboxProfile& profile,
                            SandboxKernelEnforcement& enforcement,
                            std::string& error) {
    error.clear();
    enforcement = {};
    if (!validateSandboxProfile(profile, error)) {
        return false;
    }
    if (geteuid() != 0) {
        error = "sandbox namespace setup requires root sandboxd";
        return false;
    }

    int flags = 0;
    if (profile.privateIpcNamespace) {
        flags |= CLONE_NEWIPC;
    }
    if (profile.privateNetworkNamespace) {
        flags |= CLONE_NEWNET;
    }
    if (profile.privatePidNamespace) {
        flags |= CLONE_NEWPID;
    }
    if (flags == 0 || unshare(flags) != 0) {
        error = std::string("could not create sandbox IPC, network, and PID namespaces: ") +
                std::strerror(errno);
        return false;
    }
    enforcement.ipcNamespaceReady = profile.privateIpcNamespace;
    enforcement.networkNamespaceReady = profile.privateNetworkNamespace;
    // The caller is still in the parent PID namespace at this point.  Mark it
    // ready only after the inner fork confirms it became PID 1.
    return true;
}

bool finalizeSandboxPidNamespace(const SandboxProfile& profile,
                                 SandboxKernelEnforcement& enforcement,
                                 std::string& error) {
    error.clear();
    if (!validateSandboxProfile(profile, error)) {
        return false;
    }
    if (!profile.privatePidNamespace || !enforcement.ipcNamespaceReady ||
        !enforcement.networkNamespaceReady || getpid() != 1) {
        error = "sandbox child did not become the isolated PID-namespace init process";
        return false;
    }
    enforcement.pidNamespaceReady = true;
    return true;
}

} // namespace lcl::security
