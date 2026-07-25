#include <iostream>
#include <csignal>
#include <atomic>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <cstring>
#include "core/app/app_bundle_parser.hpp"
#include "core/ipc/ipc_manager.hpp"

namespace {
    bool g_waitingMode{false};

    void openSignalHandler(int sig) {
        (void)sig;
        if (g_waitingMode) {
            std::cout << "\n[LCL Open] SIGINT received! Requesting Compositor to destroy spawned window over Unix Domain Socket...\n";
            lcl::core::IPCManager::sendClientRequest("DESTROY_LAST_WINDOW", lcl::core::kCompositorSocket, false);
        }
        _exit(130);
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: open [-w|--wait] <app_name.app | path_to_app>\n";
        return 1;
    }

    bool waitMode = false;
    std::string target;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-w" || arg == "--wait") {
            waitMode = true;
        } else if (target.empty()) {
            target = arg;
        }
    }

    if (target.empty()) {
        std::cout << "Usage: open [-w|--wait] <app_name.app | path_to_app>\n";
        return 1;
    }

    if (target.rfind(".app") == std::string::npos && target.find('/') == std::string::npos) {
        target += ".app";
    }

    if (target[0] != '/' && target.find("/home/user/Applications/") == std::string::npos) {
        std::string candidate = "/home/user/Applications/" + target;
        auto metaCand = lcl::core::AppBundleParser::parseBundle(candidate);
        if (metaCand && metaCand->valid) {
            target = candidate;
        }
    }

    auto meta = lcl::core::AppBundleParser::parseBundle(target);
    if (!meta || !meta->valid) {
        std::cerr << "[LCL Open ERROR] Invalid .app bundle: " << target << " (missing or invalid metadata.json)\n";
        return 1;
    }

    g_waitingMode = waitMode;
    std::signal(SIGINT, openSignalHandler);
    std::signal(SIGTERM, openSignalHandler);

    // Check GUI app spawning via Secure Unix Domain Socket
    if (meta->type == "gui" || meta->name == "LCL Terminal" || target.find("Terminal.app") != std::string::npos) {
        if (!waitMode) {
            std::cout << "[LCL Open] Requesting LCL Compositor to spawn GUI Window for " << meta->name << "...\n";
            std::string res = lcl::core::IPCManager::sendClientRequest("SPAWN_TERMINAL", lcl::core::kCompositorSocket, false);
            (void)res;
            std::cout << "[LCL Open SUCCESS] Sent SPAWN_TERMINAL IPC request over Unix Domain Socket.\n";
            return 0;
        } else {
            std::cout << "[LCL Open] Requesting GUI Window for " << meta->name << " (blocking mode: waiting on socket ACK)...\n";
            std::string response = lcl::core::IPCManager::sendClientRequest("SPAWN_TERMINAL_WAIT", lcl::core::kCompositorSocket, true);
            if (response == "DONE") {
                std::cout << "[LCL Open] Window closed cleanly (ACK received over Unix Socket). Returning to shell prompt.\n";
                return 0;
            } else {
                std::cout << "[LCL Open] Socket connection closed or interrupted.\n";
                return 0;
            }
        }
    }

    std::cout << "[LCL Open] Launching " << meta->name << " v" << meta->version
              << (waitMode ? " (attached/blocking mode)" : " (detached/background mode)") << "...\n";

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "[LCL Open ERROR] Failed to fork process.\n";
        return 1;
    }

    if (pid == 0) {
        // Child process
        if (!waitMode) {
            setsid();
        }
        char* const execArgv[] = { const_cast<char*>(meta->executablePath.c_str()), nullptr };
        execv(meta->executablePath.c_str(), execArgv);
        _exit(127);
    }

    if (waitMode) {
        waitpid(pid, nullptr, 0);
    } else {
        std::cout << "[LCL Open] App launched in background with PID " << pid << ".\n";
    }

    return 0;
}
