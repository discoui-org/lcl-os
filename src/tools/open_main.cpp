#include <iostream>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <cstring>
#include "core/app/app_bundle_parser.hpp"

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

    if (target[0] != '/' && target.find("/home/user/Applications/") == std::string::npos && target.find("/Applications/") == std::string::npos) {
        std::string candidate = "/home/user/Applications/" + target;
        auto metaCand = lcl::core::AppBundleParser::parseBundle(candidate);
        if (metaCand && metaCand->valid) {
            target = candidate;
        } else {
            std::string sysCand = "/Applications/" + target;
            auto metaSys = lcl::core::AppBundleParser::parseBundle(sysCand);
            if (metaSys && metaSys->valid) {
                target = sysCand;
            }
        }
    }

    auto meta = lcl::core::AppBundleParser::parseBundle(target);
    if (!meta || !meta->valid) {
        std::cerr << "[LCL Open ERROR] Invalid .app bundle: " << target << " (missing or invalid metadata.json)\n";
        return 1;
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
