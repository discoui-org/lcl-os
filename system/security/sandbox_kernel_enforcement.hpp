#pragma once

namespace lcl::security {

/**
 * Protected sandboxd-side evidence that mandatory kernel stages completed in
 * this child immediately before credential drop. It is never IPC input.
 */
struct SandboxKernelEnforcement {
    bool mountNamespaceReady{false};
    bool pidNamespaceReady{false};
    bool ipcNamespaceReady{false};
    bool networkNamespaceReady{false};
    bool noNewPrivilegesInstalled{false};
    bool seccompInstalled{false};
    bool landlockInstalled{false};
    bool directDeviceAccessDenied{false};
    bool portableResourceLimitsInstalled{false};
};

} // namespace lcl::security
