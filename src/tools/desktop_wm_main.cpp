#include <iostream>
#include <vector>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <cstring>

#include "core/ipc/ipc_manager.hpp"
#include "core/ipc/lcl_protocol.hpp"

int main() {
    std::cout << "====================================================\n"
              << "  lcl-desktop-wm v0.1.0 - Window Manager Daemon   \n"
              << "====================================================\n";

    // 1. Connect to Compositor Unix Domain Socket
    int socketFd = -1;
    for (int i = 0; i < 50; ++i) {
        socketFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (socketFd >= 0) {
            struct sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, lcl::core::kCompositorSocket, sizeof(addr.sun_path) - 1);
            if (connect(socketFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
                break;
            }
            close(socketFd);
            socketFd = -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (socketFd < 0) {
        std::cerr << "[LCL WM ERROR] Could not connect to compositor socket: " << lcl::core::kCompositorSocket << "\n";
        return 1;
    }

    std::cout << "[LCL WM] Connected to Compositor IPC socket successfully.\n";

    // 2. Register role as WINDOW_MANAGER
    lcl::protocol::LCLHeader regHeader{};
    regHeader.opcode = lcl::protocol::LCLOpcode::RegisterRole;
    regHeader.payloadSize = sizeof(lcl::protocol::LCLMsgRegisterRole);

    lcl::protocol::LCLMsgRegisterRole regMsg{};
    regMsg.role = lcl::protocol::LCLRole::WindowManager;
    std::strncpy(regMsg.clientName, "lcl-desktop-wm", sizeof(regMsg.clientName) - 1);

    lcl::protocol::sendMsgWithFd(socketFd, regHeader, &regMsg);
    std::cout << "[LCL WM] Registered as WINDOW_MANAGER role on lcl-core compositor.\n";

    // 3. Spawn initial test client application (LCL Terminal)
    std::cout << "[LCL WM] Spawning initial test client surface (LCL Terminal App)...\n";
    lcl::core::IPCManager::sendClientRequest("SPAWN_TERMINAL");

    // 4. Main Window Manager event loop
    std::cout << "[LCL WM] Window Manager active. Managing layouts, focus, and Server-Side Decorations (SSD).\n";
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    close(socketFd);
    return 0;
}
