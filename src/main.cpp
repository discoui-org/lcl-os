#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <memory>
#include <vector>
#include <algorithm>
#include <unistd.h>
#include "core/display/display_manager.hpp"
#include "core/display/display_scale.hpp"
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

    // UI DPI scale from boot args (lcl.scale=) / LCL_SCALE
    lcl::core::DisplayScale::initialize();

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

    // Initialize Compositor IPC Manager (/tmp/lcl_compositor.sock with 0600 perms)
    lcl::core::IPCManager ipcManager;
    ipcManager.initialize("/tmp/lcl_compositor.sock");

    // Dynamic Multi-Window App Registry
    std::vector<std::unique_ptr<lcl::apps::TerminalApp>> terminalApps;
    auto primaryTerminal = std::make_unique<lcl::apps::TerminalApp>();

    // Launch Desktop Session via StartupManager (Creates primary Window ID: 1)
    lcl::core::StartupManager startupManager;
    startupManager.launchDefaultSession(windowManager, *primaryTerminal);
    terminalApps.push_back(std::move(primaryTerminal));

    // Connect Input Subsystem events to Window Manager and active focused window
    bool needsRedraw = true;
    inputManager.setEventCallback([&](const lcl::core::InputEvent& ev) {
        bool windowChanged = windowManager.processInputEvent(ev);
        if (windowChanged) {
            needsRedraw = true;
        }

        // Find active focused window ID (top of z-order stack)
        const auto& windows = windowManager.getWindows();
        if (!windows.empty()) {
            uint32_t focusedId = windows.back().id;
            for (auto& app : terminalApps) {
                if (static_cast<uint32_t>(app->getWindowId()) == focusedId) {
                    app->handleInput(ev);
                    needsRedraw = true;
                    break;
                }
            }
        }
    });

    std::cout << "[LCL Core] Secure Unix Domain Socket Compositor IPC active! Press Ctrl+C to terminate.\n";

    // Frame pacing matches DRM refresh rate (fallback 60 Hz)
    uint32_t targetHz = displayManager.isInitialized() ? displayManager.getActiveDisplayMode().refreshRate : 60;
    if (targetHz == 0) targetHz = 60;
    auto targetFrameDuration = std::chrono::microseconds(1000000 / targetHz);
    std::cout << "[LCL Core] High Refresh Rate active: targeting " << targetHz << " Hz (~"
              << (1000000 / targetHz) << " us per frame budget).\n";

    uint64_t loopTicks = 0;
    auto lastBlinkCheck = std::chrono::steady_clock::now();

    while (g_running.load()) {
        auto frameStart = std::chrono::high_resolution_clock::now();

        if (inputManager.isInitialized()) {
            inputManager.dispatchEvents(renderer.getWidth(), renderer.getHeight());
        }

        // Poll authenticated IPC messages over Unix Domain Socket (SO_PEERCRED validated)
        auto ipcMsgs = ipcManager.pollMessages();
        for (const auto& msg : ipcMsgs) {
            std::cout << "[LCL IPC SECURITY] Message from authenticated Client (PID: "
                      << msg.pid << ", UID: " << msg.uid << "): '" << msg.command << "'\n";

            if (msg.command == "SPAWN_TERMINAL" || msg.command == "SPAWN_TERMINAL_WAIT") {
                using lcl::core::DisplayScale;
                int x = DisplayScale::px(80 + static_cast<int>(terminalApps.size() * 40));
                int y = DisplayScale::px(60 + static_cast<int>(terminalApps.size() * 30));
                uint32_t winId = windowManager.createWindow(
                    "LCL Terminal",
                    x,
                    y,
                    DisplayScale::px(DisplayScale::kDefaultWinW),
                    DisplayScale::px(DisplayScale::kDefaultWinH),
                    0xFF89B4FA);

                auto newApp = std::make_unique<lcl::apps::TerminalApp>();
                newApp->initialize(winId);

                if (msg.command == "SPAWN_TERMINAL_WAIT") {
                    newApp->setAckFifo(std::to_string(msg.clientFd)); // Store client FD for ACK socket response
                }
                terminalApps.push_back(std::move(newApp));
                needsRedraw = true;

                std::cout << "[LCL Compositor] Dynamically spawned GUI Terminal Window (ID: " << winId
                          << (msg.command == "SPAWN_TERMINAL_WAIT" ? " [blocking -w mode]" : "") << ") on desktop!\n";
            } else if (msg.command == "DESTROY_LAST_WINDOW") {
                if (terminalApps.size() > 1) {
                    auto& lastApp = terminalApps.back();
                    uint32_t winId = static_cast<uint32_t>(lastApp->getWindowId());
                    std::cout << "[LCL Compositor] DESTROY_LAST_WINDOW: Destroying Window ID " << winId
                              << " and terminating process group due to SIGINT cancel.\n";
                    windowManager.removeWindow(winId);
                    lastApp->shutdown();
                    terminalApps.pop_back();
                    needsRedraw = true;
                }
            }
        }

        // Synchronize closed windows from WindowManager (e.g. user clicked red close button)
        const auto& activeWindows = windowManager.getWindows();
        for (auto it = terminalApps.begin(); it != terminalApps.end(); ) {
            uint32_t winId = static_cast<uint32_t>((*it)->getWindowId());
            bool winExists = std::any_of(activeWindows.begin(), activeWindows.end(), [winId](const lcl::render::Window& w) {
                return w.id == winId;
            });

            if (!winExists) {
                std::cout << "[LCL Compositor] Window ID " << winId << " closed via UI button. Destroying process.\n";
                if (!(*it)->getAckFifo().empty()) {
                    int cFd = std::stoi((*it)->getAckFifo());
                    lcl::core::IPCManager::sendResponse(cFd, "DONE");
                }
                (*it)->shutdown();
                it = terminalApps.erase(it);
                needsRedraw = true;
            } else {
                ++it;
            }
        }

        // Update active Terminal App Surfaces & clean up exited PTY processes
        for (auto it = terminalApps.begin(); it != terminalApps.end(); ) {
            auto& app = *it;
            bool appChanged = app->update();
            if (appChanged) {
                uint32_t winId = static_cast<uint32_t>(app->getWindowId());
                for (auto& w : windowManager.getWindowsMutable()) {
                    if (w.id == winId) {
                        w.markDirty();
                        break;
                    }
                }
                needsRedraw = true;
            }

            if (!app->isAlive()) {
                uint32_t winId = static_cast<uint32_t>(app->getWindowId());
                std::cout << "[LCL Compositor] Terminal App (Window ID: " << winId << ") process exited.\n";
                if (!app->getAckFifo().empty()) {
                    int cFd = std::stoi(app->getAckFifo());
                    lcl::core::IPCManager::sendResponse(cFd, "DONE");
                }
                windowManager.removeWindow(winId);
                app->shutdown();
                it = terminalApps.erase(it);
                needsRedraw = true;
            } else {
                ++it;
            }
        }

        // 500ms Cursor Blink tick trigger
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastBlinkCheck).count() >= 500) {
            lastBlinkCheck = now;
            needsRedraw = true;
        }

        // Event-Driven & Damage Tracking: Render ONLY when dirty/event occurred
        if (needsRedraw || windowManager.isAnyWindowDirty()) {
            std::vector<lcl::render::WindowRenderContent> contents;
            contents.reserve(terminalApps.size());
            for (const auto& app : terminalApps) {
                contents.push_back(app->getRenderContent());
            }

            renderer.renderDesktop(windowManager, contents);
            renderer.swapBuffers();
            windowManager.clearAllDirty();
            needsRedraw = false;
        }

        // Dynamic frame pacing
        auto frameDuration = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - frameStart);
        if (frameDuration < targetFrameDuration) {
            std::this_thread::sleep_for(targetFrameDuration - frameDuration);
        }
        loopTicks++;
    }

    // Explicit shutdown of core subsystems & apps
    for (auto& app : terminalApps) {
        if (!app->getAckFifo().empty()) {
            int cFd = std::stoi(app->getAckFifo());
            lcl::core::IPCManager::sendResponse(cFd, "DONE");
        }
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
