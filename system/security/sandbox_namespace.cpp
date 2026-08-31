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

bool beginSandboxNamespaces(const SandboxPlatformHardening& hardening,
                            SandboxKernelEnforcement& enforcement,
                            std::string& error) {
    error.clear();
    enforcement = {};
    if (!validateSandboxPlatformHardening(hardening, error)) {
        return false;
    }
    if (geteuid() != 0) {
        error = "sandbox namespace setup requires root sandboxd";
        return false;
    }

    int flags = 0;
    if (hardening.privateIpcNamespace) {
        flags |= CLONE_NEWIPC;
    }
    if (hardening.privateNetworkNamespace) {
        flags |= CLONE_NEWNET;
    }
    if (hardening.privatePidNamespace) {
        flags |= CLONE_NEWPID;
    }
    if (flags != 0 && unshare(flags) != 0) {
        error = std::string("could not create sandbox IPC, network, and PID namespaces: ") +
                std::strerror(errno);
        return false;
    }
    enforcement.ipcNamespaceReady = hardening.privateIpcNamespace;
    enforcement.networkNamespaceReady = hardening.privateNetworkNamespace;
    // The caller is still in the parent PID namespace at this point.  Mark it
    // ready only after the inner fork confirms it became PID 1.
    return true;
}

bool finalizeSandboxPidNamespace(const SandboxPlatformHardening& hardening,
                                 SandboxKernelEnforcement& enforcement,
                                 std::string& error) {
    error.clear();
    if (!validateSandboxPlatformHardening(hardening, error)) {
        return false;
    }
    if (!hardening.privatePidNamespace) {
        return true;
    }
    if (!enforcement.ipcNamespaceReady || !enforcement.networkNamespaceReady || getpid() != 1) {
        error = "sandbox child did not become the isolated PID-namespace init process";
        return false;
    }
    enforcement.pidNamespaceReady = true;
    return true;
}

} // namespace lcl::security
