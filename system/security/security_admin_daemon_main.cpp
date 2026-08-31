#include "system/security/security_admin_daemon.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#include <thread>
#include <utility>

#include <unistd.h>

namespace {

std::atomic<bool> g_running{true};

void onSignal(int) { g_running = false; }

bool parseId(std::string_view text, std::uintmax_t& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() && value != 0 &&
           value <= static_cast<std::uintmax_t>(std::numeric_limits<uid_t>::max());
}

void usage() {
    std::cerr << "usage: lcl-securityd --session-uid <uid> --session-gid <gid>"
                 " [--socket <absolute-path>]\n";
}

} // namespace

int main(int argc, char** argv) {
    if (geteuid() != 0) {
        std::cerr << "[LCL Security ERROR] lcl-securityd must run as root\n";
        return 1;
    }

    lcl::security::SecurityAdminDaemonConfig config{};
    bool gotUid = false;
    bool gotGid = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if ((argument == "--session-uid" || argument == "--session-gid" || argument == "--socket") &&
            index + 1 < argc) {
            const std::string_view value(argv[++index]);
            if (argument == "--socket") {
                config.socketPath = value;
                continue;
            }
            std::uintmax_t parsed = 0;
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

    lcl::security::SecurityAdminDaemon daemon(std::move(config));
    std::string error;
    if (!daemon.initialize(error)) {
        std::cerr << "[LCL Security ERROR] " << error << '\n';
        return 1;
    }
    while (g_running.load()) {
        daemon.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return 0;
}
