#include <iostream>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <cstring>
#include "core/app/app_bundle_parser.hpp"
#include "core/ipc/ipc_manager.hpp"

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

    // Check GUI app spawning
    if (meta->type == "gui" || meta->name == "LCL Terminal" || target.find("Terminal.app") != std::string::npos) {
        if (!waitMode) {
            // Non-blocking mode: send IPC and exit immediately
            std::cout << "[LCL Open] Requesting LCL Compositor to spawn GUI Window for " << meta->name << "...\n";
            if (lcl::core::IPCManager::sendMessage("SPAWN_TERMINAL")) {
                std::cout << "[LCL Open SUCCESS] Sent SPAWN_TERMINAL IPC request to Compositor.\n";
                return 0;
            }
        } else {
            // Blocking mode (-w): create ACK FIFO and wait for window close!
            pid_t selfPid = getpid();
            std::string ackFifoPath = "/tmp/lcl_ipc_ack_" + std::to_string(selfPid) + ".fifo";
            unlink(ackFifoPath.c_str());
            if (mkfifo(ackFifoPath.c_str(), 0666) == 0) {
                std::string reqMsg = "SPAWN_TERMINAL_WAIT " + ackFifoPath;
                std::cout << "[LCL Open] Requesting GUI Window for " << meta->name << " (blocking mode: waiting for window close)...\n";
                if (lcl::core::IPCManager::sendMessage(reqMsg)) {
                    int ackFd = open(ackFifoPath.c_str(), O_RDONLY);
                    if (ackFd >= 0) {
                        char ackBuf[64];
                        ssize_t n = read(ackFd, ackBuf, sizeof(ackBuf) - 1);
                        (void)n;
                        close(ackFd);
                    }
                    unlink(ackFifoPath.c_str());
                    std::cout << "[LCL Open] Window closed. Returning to shell prompt.\n";
                    return 0;
                }
                unlink(ackFifoPath.c_str());
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
        // Wait for child process to complete in blocking mode (-w)
        waitpid(pid, nullptr, 0);
    } else {
        // Return immediately to shell prompt in non-blocking mode!
        std::cout << "[LCL Open] App launched in background with PID " << pid << ".\n";
    }

    return 0;
}
