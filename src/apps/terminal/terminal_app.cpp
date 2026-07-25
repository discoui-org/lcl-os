#include "apps/terminal/terminal_app.hpp"
#include <iostream>
#include <algorithm>
#include <linux/input-event-codes.h>

namespace lcl::apps {

TerminalApp::TerminalApp() = default;

TerminalApp::~TerminalApp() {
    shutdown();
}

bool TerminalApp::initialize(int windowId) {
    if (m_initialized) return true;

    m_windowId = windowId;
    std::cout << "[LCL App] Initializing LCL Terminal App (Window ID: " << m_windowId << ")...\n";

    if (!m_ptyManager.spawnShell("/bin/sh")) {
        std::cerr << "[LCL App ERROR] Failed to spawn PTY shell for Terminal App.\n";
        return false;
    }

    m_lines = {""};
    m_currentLine.clear();
    m_initialized = true;
    std::cout << "[LCL App] LCL Terminal App initialized successfully!\n";
    return true;
}

std::string TerminalApp::stripANSI(const std::string& input) {
    std::string result;
    bool inEscape = false;
    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (c == '\033') { // ESC
            inEscape = true;
            continue;
        }
        if (inEscape) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~') {
                inEscape = false;
            }
            continue;
        }
        result.push_back(c);
    }
    return result;
}

std::string TerminalApp::keycodeToASCII(uint32_t keycode, bool shift) {
    switch (keycode) {
        case KEY_ENTER: return "\n";
        case KEY_BACKSPACE: return "\b";
        case KEY_TAB: return "\t";
        case KEY_SPACE: return " ";
        case KEY_A: return shift ? "A" : "a";
        case KEY_B: return shift ? "B" : "b";
        case KEY_C: return shift ? "C" : "c";
        case KEY_D: return shift ? "D" : "d";
        case KEY_E: return shift ? "E" : "e";
        case KEY_F: return shift ? "F" : "f";
        case KEY_G: return shift ? "G" : "g";
        case KEY_H: return shift ? "H" : "h";
        case KEY_I: return shift ? "I" : "i";
        case KEY_J: return shift ? "J" : "j";
        case KEY_K: return shift ? "K" : "k";
        case KEY_L: return shift ? "L" : "l";
        case KEY_M: return shift ? "M" : "m";
        case KEY_N: return shift ? "N" : "n";
        case KEY_O: return shift ? "O" : "o";
        case KEY_P: return shift ? "P" : "p";
        case KEY_Q: return shift ? "Q" : "q";
        case KEY_R: return shift ? "R" : "r";
        case KEY_S: return shift ? "S" : "s";
        case KEY_T: return shift ? "T" : "t";
        case KEY_U: return shift ? "U" : "u";
        case KEY_V: return shift ? "V" : "v";
        case KEY_W: return shift ? "W" : "w";
        case KEY_X: return shift ? "X" : "x";
        case KEY_Y: return shift ? "Y" : "y";
        case KEY_Z: return shift ? "Z" : "z";
        case KEY_1: return shift ? "!" : "1";
        case KEY_2: return shift ? "@" : "2";
        case KEY_3: return shift ? "#" : "3";
        case KEY_4: return shift ? "$" : "4";
        case KEY_5: return shift ? "%" : "5";
        case KEY_6: return shift ? "^" : "6";
        case KEY_7: return shift ? "&" : "7";
        case KEY_8: return shift ? "*" : "8";
        case KEY_9: return shift ? "(" : "9";
        case KEY_0: return shift ? ")" : "0";
        case KEY_MINUS: return shift ? "_" : "-";
        case KEY_EQUAL: return shift ? "+" : "=";
        case KEY_DOT: return shift ? ">" : ".";
        case KEY_COMMA: return shift ? "<" : ",";
        case KEY_SLASH: return shift ? "?" : "/";
        default: return "";
    }
}

void TerminalApp::handleInput(const core::InputEvent& ev) {
    if (!m_initialized) return;

    if (ev.type == core::InputEventType::KeyboardKey) {
        if (ev.key == KEY_LEFTSHIFT || ev.key == KEY_RIGHTSHIFT) {
            m_shiftPressed = ev.pressed;
        }
        if (ev.key == KEY_LEFTCTRL || ev.key == KEY_RIGHTCTRL) {
            m_ctrlPressed = ev.pressed;
        }

        if (ev.pressed && !ev.isRepeat) {
            // Handle Ctrl+L shortcut to clear screen
            if (m_ctrlPressed && ev.key == KEY_L) {
                clearBuffer();
                m_ptyManager.writeInput("\n");
                return;
            }

            std::string ascii = keycodeToASCII(ev.key, m_shiftPressed);
            if (!ascii.empty()) {
                m_ptyManager.writeInput(ascii);
            }
        }
    }
}

void TerminalApp::clearBuffer() {
    m_lines.clear();
    m_lines.push_back("");
    m_currentLine.clear();
}

void TerminalApp::update() {
    if (!m_initialized) return;

    std::string rawOut = m_ptyManager.readOutput();
    if (rawOut.empty()) return;

    // Check for ANSI screen clear codes (\033[2J or \033[H) sent by clear command
    if (rawOut.find("\033[2J") != std::string::npos || rawOut.find("\033[H") != std::string::npos) {
        clearBuffer();
    }

    std::string clean = stripANSI(rawOut);
    for (char ch : clean) {
        if (ch == '\r') continue;
        if (ch == '\n') {
            if (!m_lines.empty()) {
                m_lines.back() = m_currentLine;
            } else {
                m_lines.push_back(m_currentLine);
            }
            m_currentLine.clear();
            m_lines.push_back("");
        } else if (ch == '\b' || ch == 0x7F) {
            if (!m_currentLine.empty()) m_currentLine.pop_back();
        } else if (static_cast<unsigned char>(ch) >= 32 && static_cast<unsigned char>(ch) <= 126) {
            m_currentLine.push_back(ch);
        }
    }

    if (!m_lines.empty()) {
        m_lines.back() = m_currentLine;
    } else {
        m_lines.push_back(m_currentLine);
    }
}

void TerminalApp::shutdown() {
    if (!m_initialized) return;
    std::cout << "[LCL App] Shutting down LCL Terminal App...\n";
    m_ptyManager.shutdown();
    m_initialized = false;
}

} // namespace lcl::apps
