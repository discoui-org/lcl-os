#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <iostream>
#include <limits>
#include <string_view>
#include <thread>
#include <utility>

#include <unistd.h>

#include "system/security/sandbox_daemon.hpp"
#include "system/security/sandbox_system_application_registry.hpp"

namespace {

std::atomic<bool> gRunning{true};

void onSignal(int) { gRunning = false; }

bool parseId(std::string_view value, unsigned long& parsed) {
    if (value.empty()) {
        return false;
    }
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return error == std::errc{} && end == value.data() + value.size() &&
           parsed <= std::numeric_limits<unsigned int>::max();
}

bool parsePlatformMode(std::string_view value, lcl::security::SandboxPlatformMode& mode) {
    if (value == "linux-full") {
        mode = lcl::security::SandboxPlatformMode::LinuxFull;
        return true;
    }
    if (value == "android-capability") {
        mode = lcl::security::SandboxPlatformMode::AndroidCapability;
        return true;
    }
    return false;
}

void usage() {
    std::cerr << "usage: lcl-sandboxd --session-uid <uid> --session-gid <gid>"
                 " [--socket <absolute-path>]"
                 " [--platform linux-full|android-capability]\n";
}

} // namespace

int main(int argc, char** argv) {
    if (geteuid() != 0) {
        std::cerr << "[LCL Sandbox ERROR] lcl-sandboxd must run as root\n";
        return 1;
    }

    lcl::security::SandboxDaemonConfig config{};
    bool gotUid = false;
    bool gotGid = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if ((argument == "--session-uid" || argument == "--session-gid" || argument == "--socket" ||
             argument == "--platform") &&
            index + 1 < argc) {
            const std::string_view value(argv[++index]);
            if (argument == "--socket") {
                config.socketPath = value;
                continue;
            }
            if (argument == "--platform") {
                if (!parsePlatformMode(value, config.platformMode)) {
                    usage();
                    return 2;
                }
                continue;
            }
            unsigned long parsed = 0;
            if (!parseId(value, parsed)) {
                usage();
                return 2;
            }
            if (argument == "--session-uid") {
                config.sessionUid = static_cast<uid_t>(parsed);
                gotUid = true;
            } else {
                config.sessionGid = static_cast<gid_t>(parsed);
                gotGid = true;
            }
            continue;
        }
        usage();
        return 2;
    }
    if (!gotUid || !gotGid) {
        usage();
        return 2;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);

    lcl::security::SandboxDaemon daemon(std::move(config));
    std::string error;
    if (!daemon.initialize(error)) {
        std::cerr << "[LCL Sandbox ERROR] " << error << "\n";
        return 1;
    }
    lcl::security::SandboxSystemApplicationRegistry systemApplications;
    if (!systemApplications.registerSystemApplications(daemon, error)) {
        std::cerr << "[LCL Sandbox ERROR] " << error << "\n";
        return 1;
    }
    std::string lastEndpointRefreshError;
    while (gRunning.load()) {
        daemon.poll();
        if (daemon.runtimeEndpointRefreshRequired()) {
            // Reopen the protected graphics sockets and replace every
            // system-image material record.  Until this succeeds sandboxd
            // rejects launches and has already killed children that carried
            // stale private bind mounts.
            if (!systemApplications.registerSystemApplications(daemon, error) ||
                !daemon.completeRuntimeEndpointRefresh(error)) {
                if (error != lastEndpointRefreshError) {
                    std::cerr << "[LCL Sandbox ERROR] graphics endpoint refresh failed: " << error
                              << "\n";
                    lastEndpointRefreshError = error;
                }
            } else {
                lastEndpointRefreshError.clear();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return 0;
}
