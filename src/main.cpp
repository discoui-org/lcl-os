#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <sstream>
#include "core/display/display_manager.hpp"
#include "core/input/input_manager.hpp"
#include "core/terminal/pty_manager.hpp"
#include "render/renderer.hpp"
#include "render/window_manager.hpp"

namespace {
    // Atomic signal flag for thread-safe graceful shutdown
    std::atomic<bool> g_running{true};

    void signalHandler(int signal) {
        if (signal == SIGINT || signal == SIGTERM) {
            std::cout << "\n[LCL Core] Signal received (" << signal << "). Initiating graceful shutdown...\n";
            g_running.store(false);
        }
    }

    std::string keycodeToASCII(uint32_t keycode, bool shift) {
        switch (keycode) {
            case 28: return "\n"; // KEY_ENTER -> send \n to trigger shell command execution!
            case 14: return "\b"; // KEY_BACKSPACE
            case 15: return "\t"; // KEY_TAB
            case 57: return " ";  // KEY_SPACE
            case 30: return shift ? "A" : "a";
            case 48: return shift ? "B" : "b";
            case 46: return shift ? "C" : "c";
            case 32: return shift ? "D" : "d";
            case 18: return shift ? "E" : "e";
            case 33: return shift ? "F" : "f";
            case 34: return shift ? "G" : "g";
            case 35: return shift ? "H" : "h";
            case 23: return shift ? "I" : "i";
            case 36: return shift ? "J" : "j";
            case 37: return shift ? "K" : "k";
            case 38: return shift ? "L" : "l";
            case 50: return shift ? "M" : "m";
            case 49: return shift ? "N" : "n";
            case 24: return shift ? "O" : "o";
            case 25: return shift ? "P" : "p";
            case 16: return shift ? "Q" : "q";
            case 19: return shift ? "R" : "r";
            case 31: return shift ? "S" : "s";
            case 20: return shift ? "T" : "t";
            case 22: return shift ? "U" : "u";
            case 47: return shift ? "V" : "v";
            case 17: return shift ? "W" : "w";
            case 45: return shift ? "X" : "x";
            case 21: return shift ? "Y" : "y";
            case 44: return shift ? "Z" : "z";
            case 2:  return shift ? "!" : "1";
            case 3:  return shift ? "@" : "2";
            case 4:  return shift ? "#" : "3";
            case 5:  return shift ? "$" : "4";
            case 6:  return shift ? "%" : "5";
            case 7:  return shift ? "^" : "6";
            case 8:  return shift ? "&" : "7";
            case 9:  return shift ? "*" : "8";
            case 10: return shift ? ")" : "0";
            case 11: return shift ? "(" : "9";
            case 12: return shift ? "_" : "-";
            case 13: return shift ? "+" : "=";
            case 52: return shift ? ">" : ".";
            case 51: return shift ? "<" : ",";
            case 53: return shift ? "?" : "/";
            default: return "";
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

    // Initialize PTY Subprocess Manager
    lcl::core::PTYManager ptyManager;
    ptyManager.spawnShell("/bin/sh");

    // Terminal display buffer lines
    std::vector<std::string> terminalLines = {
        "LCL OS Interactive Terminal v0.1.0",
        "Connected to PTY master (/bin/sh)",
        "Type commands (ls, pwd, uname -a, clear) and press Enter:",
        ""
    };
    std::string currentLine;
    bool shiftPressed = false;

    // Connect Input Subsystem events to Window Manager and PTY Terminal
    inputManager.setEventCallback([&](const lcl::core::InputEvent& ev) {
        windowManager.processInputEvent(ev);

        // Keyboard tracking and PTY transmission
        if (ev.type == lcl::core::InputEventType::KeyboardKey) {
            if (ev.key == 42 || ev.key == 54) { // LEFTSHIFT / RIGHTSHIFT
                shiftPressed = ev.pressed;
            }

            if (ev.pressed) {
                std::string ascii = keycodeToASCII(ev.key, shiftPressed);
                if (!ascii.empty()) {
                    ptyManager.writeInput(ascii);
                }
            }
        }
    });

    std::cout << "[LCL Core] Interactive Terminal & Window Manager active! Press Ctrl+C to terminate.\n";

    // Main 60 FPS interactive event & render loop
    uint64_t loopTicks = 0;
    while (g_running.load()) {
        if (inputManager.isInitialized()) {
            inputManager.dispatchEvents(renderer.getWidth(), renderer.getHeight());
        }

        // Read async shell PTY output
        std::string ptyOut = ptyManager.readOutput();
        if (!ptyOut.empty()) {
            for (char ch : ptyOut) {
                if (ch == '\r') continue;
                if (ch == '\n') {
                    terminalLines.push_back(currentLine);
                    currentLine.clear();
                } else if (ch == '\b' || ch == 0x7F) {
                    if (!currentLine.empty()) currentLine.pop_back();
                } else {
                    currentLine.push_back(ch);
                }
            }
            if (!currentLine.empty()) {
                if (terminalLines.empty()) terminalLines.push_back(currentLine);
                else terminalLines.back() = currentLine;
            }
        }

        // Render interactive desktop with active windows, PTY terminal output, & mouse cursor
        renderer.renderDesktop(windowManager, terminalLines);
        renderer.swapBuffers();

        // ~60 FPS frame rate target
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        loopTicks++;
    }

    // Explicit shutdown of core subsystems
    ptyManager.shutdown();
    renderer.shutdown();
    inputManager.shutdown();
    displayManager.shutdown();

    std::cout << "[LCL Core] Clean shutdown complete. Total event loop ticks: " << loopTicks << "\n";
    return 0;
}
