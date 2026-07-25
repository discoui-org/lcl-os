#include "apps/terminal/terminal_app.hpp"
#include <iostream>
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
    m_writePos = 0;
    m_initialized = true;

    // Set PTY winsize to match visible terminal window columns and rows
    m_ptyManager.resizeWindow(57, 17);

    std::cout << "[LCL App] LCL Terminal App initialized successfully!\n";
    return true;
}

render::WindowRenderContent TerminalApp::getRenderContent() const {
    render::WindowRenderContent content;
    content.windowId = static_cast<uint32_t>(m_windowId);
    content.lines = m_lines;
    content.suggestion = "";
    content.cursorCol = m_writePos;

    auto now = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastInputTime).count();
    // Solid cursor while actively typing (< 600ms), resumes 500ms blink when idle
    content.forceCursorSolid = (elapsedMs < 600);
    return content;
}

void TerminalApp::clearBuffer() {
    m_lines.clear();
    m_lines.push_back("");
    m_writePos = 0;
}

void TerminalApp::update() {
    if (!m_initialized) return;

    std::string rawOut = m_ptyManager.readOutput();
    if (rawOut.empty()) return;

    // Detect ANSI clear-screen: full reset
    if (rawOut.find("\033[2J") != std::string::npos) {
        clearBuffer();
        return;
    }

    if (m_lines.empty()) m_lines.push_back("");

    size_t i = 0;
    while (i < rawOut.size()) {
        unsigned char c = rawOut[i];

        // --- CSI escape sequence (\033[ ... <cmd>) ---
        if (c == '\033' && i + 1 < rawOut.size() && rawOut[i + 1] == '[') {
            i += 2; // skip ESC [

            // Consume all parameter bytes (0x20..0x3F: digits, semicolons, etc.)
            std::string paramStr;
            while (i < rawOut.size() && rawOut[i] >= 0x20 && rawOut[i] <= 0x3F) {
                paramStr.push_back(rawOut[i]);
                ++i;
            }

            // Consume final command byte (0x40..0x7E: 'm', 'K', 'D', 'C', 'P', etc.)
            if (i < rawOut.size()) {
                char cmd = rawOut[i++];
                int n = 1;
                if (!paramStr.empty()) {
                    try { n = std::stoi(paramStr); } catch (...) { n = 1; }
                }
                if (n <= 0) n = 1;

                std::string& line = m_lines.back();

                if (cmd == 'K') {
                    // Erase to end of line from writePos
                    if (m_writePos <= static_cast<int>(line.size())) {
                        line.resize(m_writePos);
                    }
                } else if (cmd == 'D') {
                    // Cursor/write-head left n UTF-8 codepoints
                    for (int j = 0; j < n && m_writePos > 0; ++j) {
                        --m_writePos;
                        while (m_writePos > 0 &&
                               (static_cast<unsigned char>(line[m_writePos]) & 0xC0) == 0x80) {
                            --m_writePos;
                        }
                    }
                } else if (cmd == 'C') {
                    // Cursor/write-head right n UTF-8 codepoints
                    int lineSize = static_cast<int>(line.size());
                    for (int j = 0; j < n && m_writePos < lineSize; ++j) {
                        unsigned char lc = static_cast<unsigned char>(line[m_writePos]);
                        size_t charLen = 1;
                        if      ((lc & 0xE0) == 0xC0) charLen = 2;
                        else if ((lc & 0xF0) == 0xE0) charLen = 3;
                        else if ((lc & 0xF8) == 0xF0) charLen = 4;
                        m_writePos = std::min(m_writePos + static_cast<int>(charLen), lineSize);
                    }
                } else if (cmd == 'P') {
                    // Delete n chars at write-head
                    int del = std::min(n, static_cast<int>(line.size()) - m_writePos);
                    if (del > 0) line.erase(m_writePos, del);
                }
                // Colors/SGR ('m'), cursor pos ('H','f'), etc. are safely consumed and ignored
            }
            continue;
        }

        // --- Non-CSI escape: skip until terminator ---
        if (c == '\033') {
            ++i;
            while (i < rawOut.size()) {
                char ec = rawOut[i++];
                if ((ec >= 'A' && ec <= 'Z') || (ec >= 'a' && ec <= 'z') || ec == '~') break;
            }
            continue;
        }

        if (c == '\r') {
            // Carriage return: reset write-head to start of line, DO NOT erase text
            m_writePos = 0;
            ++i;
            continue;
        }

        if (c == '\n') {
            // Newline: advance to next line
            m_lines.push_back("");
            m_writePos = 0;
            ++i;
            continue;
        }

        if (c == '\b') {
            // Backspace: move write-head left one UTF-8 codepoint
            std::string& line = m_lines.back();
            if (m_writePos > 0) {
                --m_writePos;
                while (m_writePos > 0 &&
                       (static_cast<unsigned char>(line[m_writePos]) & 0xC0) == 0x80) {
                    --m_writePos;
                }
            }
            ++i;
            continue;
        }

        // --- Printable / UTF-8 character: overwrite at write-head ---
        if (c >= 32) {
            size_t seqLen = 1;
            if      ((c & 0xE0) == 0xC0) seqLen = 2;
            else if ((c & 0xF0) == 0xE0) seqLen = 3;
            else if ((c & 0xF8) == 0xF0) seqLen = 4;
            seqLen = std::min(seqLen, rawOut.size() - i);

            std::string& line = m_lines.back();
            std::string ch = rawOut.substr(i, seqLen);
            i += seqLen;

            if (m_writePos < static_cast<int>(line.size())) {
                // Overwrite existing byte(s) — handle the case where current char
                // at writePos may be a different byte-length UTF-8 sequence
                unsigned char existing = static_cast<unsigned char>(line[m_writePos]);
                size_t existLen = 1;
                if      ((existing & 0xE0) == 0xC0) existLen = 2;
                else if ((existing & 0xF0) == 0xE0) existLen = 3;
                else if ((existing & 0xF8) == 0xF0) existLen = 4;
                line.replace(m_writePos, existLen, ch);
            } else {
                line.append(ch);
            }
            m_writePos += static_cast<int>(seqLen);
            continue;
        }

        ++i; // skip unhandled control char
    }
}

void TerminalApp::handleInput(const core::InputEvent& ev) {
    if (!m_initialized) return;

    if (ev.type == core::InputEventType::KeyboardKey) {
        m_lastInputTime = std::chrono::steady_clock::now(); // Reset typing timer on keypress
        if (ev.key == KEY_LEFTSHIFT || ev.key == KEY_RIGHTSHIFT) {
            m_shiftPressed = ev.pressed;
            return;
        }
        if (ev.key == KEY_LEFTCTRL || ev.key == KEY_RIGHTCTRL) {
            m_ctrlPressed = ev.pressed;
            return;
        }
        if (!ev.pressed) return; // Only act on key down
        std::string seq = keycodeToASCII(ev.key, m_shiftPressed);
        if (!seq.empty()) m_ptyManager.writeInput(seq);
    }
}

std::string TerminalApp::keycodeToASCII(uint32_t keycode, bool shift) {
    switch (keycode) {
        case KEY_ENTER: return "\n";
        case KEY_BACKSPACE: return "\x7F";
        case KEY_TAB: return "\t";
        case KEY_SPACE: return " ";
        case KEY_UP: return "\033[A";
        case KEY_DOWN: return "\033[B";
        case KEY_LEFT: return "\033[D";
        case KEY_RIGHT: return "\033[C";
        case KEY_HOME: return "\033[H";
        case KEY_END: return "\033[F";
        case KEY_A: return m_ctrlPressed ? "\x01" : (shift ? "A" : "a");
        case KEY_B: return shift ? "B" : "b";
        case KEY_C: return m_ctrlPressed ? "\x03" : (shift ? "C" : "c");
        case KEY_D: return m_ctrlPressed ? "\x04" : (shift ? "D" : "d");
        case KEY_E: return m_ctrlPressed ? "\x05" : (shift ? "E" : "e");
        case KEY_F: return shift ? "F" : "f";
        case KEY_G: return shift ? "G" : "g";
        case KEY_H: return shift ? "H" : "h";
        case KEY_I: return shift ? "I" : "i";
        case KEY_J: return shift ? "J" : "j";
        case KEY_K: return m_ctrlPressed ? "\x0B" : (shift ? "K" : "k");
        case KEY_L: return m_ctrlPressed ? "\x0C" : (shift ? "L" : "l");
        case KEY_M: return shift ? "M" : "m";
        case KEY_N: return shift ? "N" : "n";
        case KEY_O: return shift ? "O" : "o";
        case KEY_P: return shift ? "P" : "p";
        case KEY_Q: return shift ? "Q" : "q";
        case KEY_R: return shift ? "R" : "r";
        case KEY_S: return shift ? "S" : "s";
        case KEY_T: return shift ? "T" : "t";
        case KEY_U: return m_ctrlPressed ? "\x15" : (shift ? "U" : "u");
        case KEY_V: return shift ? "V" : "v";
        case KEY_W: return m_ctrlPressed ? "\x17" : (shift ? "W" : "w");
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
        case KEY_LEFTBRACE: return shift ? "{" : "[";
        case KEY_RIGHTBRACE: return shift ? "}" : "]";
        case KEY_BACKSLASH: return shift ? "|" : "\\";
        case KEY_SEMICOLON: return shift ? ":" : ";";
        case KEY_APOSTROPHE: return shift ? "\"" : "'";
        case KEY_GRAVE: return shift ? "~" : "`";
        case KEY_COMMA: return shift ? "<" : ",";
        case KEY_DOT: return shift ? ">" : ".";
        case KEY_SLASH: return shift ? "?" : "/";
        default: return "";
    }
}

void TerminalApp::shutdown() {
    if (!m_initialized) return;
    std::cout << "[LCL App] Shutting down LCL Terminal App (Window ID: " << m_windowId << ")...\n";
    m_ptyManager.shutdown();
    m_initialized = false;
}

} // namespace lcl::apps
