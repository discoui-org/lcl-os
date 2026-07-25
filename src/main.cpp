#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <memory>
#include <vector>
#include "core/display/display_manager.hpp"
#include "core/input/input_manager.hpp"
#include "core/ipc/ipc_manager.hpp"
#include "core/session/startup_manager.hpp"
#include "render/renderer.hpp"
#include "render/window_manager.hpp"
#include "apps/terminal/terminal_app.hpp"

namespace {
    // Atomic signal flag for thread-safe graceful shutdown
    std::atomic<bool> g_running{true};

    void signalHandler(int signal) {
        if (signal == SIGINT || signal == SIGTERM) {
            std::cout << "\n[LCL Core] Signal received (" << signal << "). Initiating graceful shutdown...\n";
            g_running.store(false);
        }
    }
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    // Register signal handlers for graceful shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::cout << "====================================================\n";
    std::cout << "  LCL Core Linux (LCL) v0.1.0 - Core Engine\n";
    std::cout << "  Architecture: Direct DRM/KMS & evdev (No X11/Wayland)\n";
    std::cout << "  C++ Standard: C++20\n";
    std::cout << "====================================================\n";
    std::cout << "[LCL Core] Initializing core subsystem skeleton...\n";

    // Initialize DRM/KMS Display Subsystem
    lcl::core::DisplayManager displayManager;
    bool displayReady = displayManager.initialize("/dev/dri/card0");
    if (!displayReady) {
        std::cout << "[LCL Core] Display subsystem running in fallback/skeleton mode.\n";
    }

    // Initialize libinput / evdev Input Subsystem
    lcl::core::InputManager inputManager;
    bool inputReady = inputManager.initialize("seat0");
    if (!inputReady) {
        std::cout << "[LCL Core] Input subsystem running in fallback/skeleton mode.\n";
    }

    // Initialize Renderer Engine Subsystem
    lcl::render::Renderer renderer;
    bool renderReady = renderer.initialize(&displayManager);
    if (!renderReady) {
        std::cout << "[LCL Core] Renderer running in fallback mode.\n";
    }

    // Initialize Window Manager Engine (starts with 0 dummy windows)
    lcl::render::WindowManager windowManager;
    windowManager.initialize(renderer.getWidth(), renderer.getHeight());

    // Initialize Compositor IPC Manager (/tmp/lcl_ipc.fifo)
    lcl::core::IPCManager ipcManager;
    ipcManager.initialize("/tmp/lcl_ipc.fifo");

    // Dynamic Multi-Window App Registry
    std::vector<std::unique_ptr<lcl::apps::TerminalApp>> terminalApps;
    auto primaryTerminal = std::make_unique<lcl::apps::TerminalApp>();

    // Launch Desktop Session via StartupManager (Creates primary Window ID: 1)
    lcl::core::StartupManager startupManager;
    startupManager.launchDefaultSession(windowManager, *primaryTerminal);
    terminalApps.push_back(std::move(primaryTerminal));

    // Connect Input Subsystem events to Window Manager and Terminal App Surfaces
    inputManager.setEventCallback([&](const lcl::core::InputEvent& ev) {
        windowManager.processInputEvent(ev);

        // Find active focused window ID
        const auto& windows = windowManager.getWindows();
        if (!windows.empty()) {
            uint32_t focusedId = windows.back().id; // Top window in z-order
            for (auto& app : terminalApps) {
                if (static_cast<uint32_t>(app->getWindowId()) == focusedId) {
                    app->handleInput(ev);
                    break;
                }
            }
        }
    });

    std::cout << "[LCL Core] Multi-Window Compositor & IPC active! Press Ctrl+C to terminate.\n";

    // Main 60 FPS interactive event & render loop
    uint64_t loopTicks = 0;
    while (g_running.load()) {
        if (inputManager.isInitialized()) {
            inputManager.dispatchEvents(renderer.getWidth(), renderer.getHeight());
        }

        // Poll IPC messages from CLI apps (e.g. open Terminal.app)
        auto ipcMsgs = ipcManager.pollMessages();
        for (const auto& msg : ipcMsgs) {
            if (msg == "SPAWN_TERMINAL") {
                int x = 80 + static_cast<int>(terminalApps.size() * 40);
                int y = 60 + static_cast<int>(terminalApps.size() * 30);
                uint32_t winId = windowManager.createWindow("LCL Terminal", x, y, 640, 480, 0xFF89B4FA);

                auto newApp = std::make_unique<lcl::apps::TerminalApp>();
                newApp->initialize(winId);
                terminalApps.push_back(std::move(newApp));

                std::cout << "[LCL Compositor] Dynamically spawned new GUI Terminal Window (ID: " << winId << ") on desktop!\n";
            }
        }

        // Update active Terminal App Surfaces (poll PTY output)
        for (auto& app : terminalApps) {
            app->update();
        }

        // Render desktop with active windows & primary app lines
        const auto& lines = terminalApps.empty() ? std::vector<std::string>{} : terminalApps.front()->getLines();
        renderer.renderDesktop(windowManager, lines);
        renderer.swapBuffers();

        // ~60 FPS frame rate target
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        loopTicks++;
    }

    // Explicit shutdown of core subsystems & apps
    for (auto& app : terminalApps) {
        app->shutdown();
    }
    terminalApps.clear();

    ipcManager.shutdown();
    renderer.shutdown();
    inputManager.shutdown();
    displayManager.shutdown();

    std::cout << "[LCL Core] Clean shutdown complete. Total event loop ticks: " << loopTicks << "\n";
    return 0;
}
