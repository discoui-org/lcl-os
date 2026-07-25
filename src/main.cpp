#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include "core/display/display_manager.hpp"
#include "core/input/input_manager.hpp"
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

    // Initialize Window Manager Engine
    lcl::render::WindowManager windowManager;
    windowManager.initialize(renderer.getWidth(), renderer.getHeight());

    // Instantiate App-ified LCL Terminal Application Module
    lcl::apps::TerminalApp terminalApp;
    terminalApp.initialize(1);

    // Connect Input Subsystem events to Window Manager and Terminal App Surface
    inputManager.setEventCallback([&](const lcl::core::InputEvent& ev) {
        windowManager.processInputEvent(ev);
        terminalApp.handleInput(ev);
    });

    std::cout << "[LCL Core] Modular App Architecture active! Press Ctrl+C to terminate.\n";

    // Main 60 FPS interactive event & render loop
    uint64_t loopTicks = 0;
    while (g_running.load()) {
        if (inputManager.isInitialized()) {
            inputManager.dispatchEvents(renderer.getWidth(), renderer.getHeight());
        }

        // Update Terminal App Surface (poll PTY output)
        terminalApp.update();

        // Render interactive desktop with active windows, Terminal App Surface, & mouse cursor
        renderer.renderDesktop(windowManager, terminalApp.getLines());
        renderer.swapBuffers();

        // ~60 FPS frame rate target
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        loopTicks++;
    }

    // Explicit shutdown of core subsystems & apps
    terminalApp.shutdown();
    renderer.shutdown();
    inputManager.shutdown();
    displayManager.shutdown();

    std::cout << "[LCL Core] Clean shutdown complete. Total event loop ticks: " << loopTicks << "\n";
    return 0;
}
