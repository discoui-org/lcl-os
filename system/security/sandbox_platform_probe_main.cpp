#include <iostream>

#include "system/security/sandbox_platform_probe.hpp"

int main() {
    const lcl::security::SandboxPlatformCapabilities capabilities =
        lcl::security::probeSandboxPlatformCapabilities();
    std::cout << lcl::security::formatSandboxPlatformCapabilities(capabilities);
    return 0;
}
