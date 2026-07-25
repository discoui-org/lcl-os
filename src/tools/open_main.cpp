#include <iostream>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "core/app/app_bundle_parser.hpp"
#include "core/ipc/ipc_manager.hpp"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: open <app_name.app | path_to_app>\n";
        return 1;
    }

    std::string target = argv[1];
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

    // Check if this is a GUI application (e.g. Terminal.app or type: gui)
    if (meta->type == "gui" || meta->name == "LCL Terminal" || target.find("Terminal.app") != std::string::npos) {
        std::cout << "[LCL Open] Requesting LCL Compositor to spawn NEW GUI Window for " << meta->name << "...\n";
        if (lcl::core::IPCManager::sendMessage("SPAWN_TERMINAL")) {
            std::cout << "[LCL Open SUCCESS] Sent SPAWN_TERMINAL IPC request to Compositor.\n";
            return 0;
        } else {
            std::cout << "[LCL Open WARNING] Compositor IPC not responding. Executing binary in fallback mode...\n";
        }
    }

    std::cout << "[LCL Open] Launching " << meta->name << " v" << meta->version << " (" << meta->executablePath << ")...\n";

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "[LCL Open ERROR] Failed to fork process.\n";
        return 1;
    }

    if (pid == 0) {
        // Child process
        char* const execArgv[] = { const_cast<char*>(meta->executablePath.c_str()), nullptr };
        execv(meta->executablePath.c_str(), execArgv);
        _exit(127);
    }

    // Wait for child process to complete execution
    waitpid(pid, nullptr, 0);
    return 0;
}
