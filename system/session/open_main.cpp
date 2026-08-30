#include <iostream>
#include <string>

#include "system/session/session_client.hpp"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: open [-w|--wait] <app-id | app-name.app | app-bundle-path>\n";
        return 1;
    }

    bool waitForExit = false;
    std::string target;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "-w" || argument == "--wait") {
            waitForExit = true;
        } else if (target.empty()) {
            target = argument;
        }
    }
    if (target.empty()) {
        std::cerr << "Usage: open [-w|--wait] <app-id | app-name.app | app-bundle-path>\n";
        return 1;
    }

    lcl::session::SessionClient client;
    if (!client.connect()) {
        std::cerr << "[LCL Open ERROR] lcl-sessiond is unavailable. Start the desktop session first.\n";
        return 1;
    }

    lcl::session::LaunchResponse response;
    std::string error;
    if (!client.launch({target, waitForExit}, response, error)) {
        std::cerr << "[LCL Open ERROR] " << error << "\n";
        return 1;
    }

    std::cout << "[LCL Open] Launched " << response.appId << " as instance "
              << response.instanceId << " (PID " << response.pid << ").\n";
    if (!waitForExit) return 0;

    lcl::session::ProcessExited exited;
    if (!client.waitForExit(response.instanceId, exited, error)) {
        std::cerr << "[LCL Open ERROR] Could not wait for instance "
                  << response.instanceId << ": " << error << "\n";
        return 1;
    }
    return exited.exitCode;
}
