#include "apps/terminal/terminal_app.hpp"
#include "platforms/common/keyboard_mapper.hpp"
#include <iostream>

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

bool TerminalApp::shouldDrawSolidCursor() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastInputTime).count();
    // Solid cursor while actively typing (< 600ms), resumes 500ms blink when idle
    return elapsedMs < 600;
}

void TerminalApp::clearBuffer() {
    m_lines.clear();
    m_lines.push_back("");
    m_writePos = 0;
}

bool TerminalApp::update() {
    if (!m_initialized) return false;

    std::string rawOut = m_ptyManager.readOutput();
    if (rawOut.empty()) return false;

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

            // Consume final command byte (0x40..0x7E: 'm', 'K', 'D', 'C', 'P', 'J', 'H', etc.)
            if (i < rawOut.size()) {
                char cmd = rawOut[i++];
                int n = 1;
                if (!paramStr.empty()) {
                    try { n = std::stoi(paramStr); } catch (...) { n = 1; }
                }
                if (n <= 0) n = 1;

                std::string& line = m_lines.back();

                if (cmd == 'J') {
                    // Erase display sequence (e.g. \033[2J or \033[3J)
                    clearBuffer();
                } else if (cmd == 'H' || cmd == 'f') {
                    // Cursor position reset (e.g. \033[H)
                    m_writePos = 0;
                } else if (cmd == 'K') {
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
                // Colors/SGR ('m'), etc. are safely consumed and ignored
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
    return true;
}

void TerminalApp::resize(int width, int height) {
    if (!m_initialized) return;

    constexpr int cellW = 8;
    constexpr int cellH = 16;
    constexpr int pad = 8;

    int cols = std::max(1, (width - pad * 2) / cellW);
    int rows = std::max(1, (height - pad * 2) / cellH);

    m_ptyManager.resizeWindow(cols, rows);
}

void TerminalApp::handleInput(const lcl::platform::RawInputEvent& ev) {
    if (!m_initialized) return;

    if (ev.type == lcl::platform::RawInputEventType::KeyboardKey) {
        if (!ev.pressed) return; // Only act on key down
        m_lastInputTime = std::chrono::steady_clock::now();

        std::string seq;
        if (ev.codepoint != 0) {
            char32_t cp = ev.codepoint;
            if (cp <= 0x7F) {
                seq.push_back(static_cast<char>(cp));
            } else if (cp <= 0x7FF) {
                seq.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
                seq.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp <= 0xFFFF) {
                seq.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
                seq.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                seq.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp <= 0x10FFFF) {
                seq.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
                seq.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                seq.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                seq.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        } else {
            seq = lcl::platform::KeyboardMapper::toUTF8(ev.key, ev.modifiers);
        }

        if (seq.empty()) {
            // Check ANSI escape sequences for navigation keys (Up, Down, Left, Right, Home, End)
            switch (ev.key) {
                case lcl::platform::PhysicalKey::ArrowUp:    seq = "\033[A"; break;
                case lcl::platform::PhysicalKey::ArrowDown:  seq = "\033[B"; break;
                case lcl::platform::PhysicalKey::ArrowLeft:  seq = "\033[D"; break;
                case lcl::platform::PhysicalKey::ArrowRight: seq = "\033[C"; break;
                case lcl::platform::PhysicalKey::Home:       seq = "\033[H"; break;
                case lcl::platform::PhysicalKey::End:        seq = "\033[F"; break;
                case lcl::platform::PhysicalKey::PageUp:     seq = "\033[5~"; break;
                case lcl::platform::PhysicalKey::PageDown:   seq = "\033[6~"; break;
                case lcl::platform::PhysicalKey::Insert:     seq = "\033[2~"; break;
                case lcl::platform::PhysicalKey::Delete:     seq = "\033[3~"; break;
                default: break;
            }
        }

        handleText(seq);
    }
}

void TerminalApp::handleKey(lcl::platform::PhysicalKey key, bool pressed, uint8_t modifiers, char32_t codepoint) {
    if (!m_initialized || !pressed) return;
    lcl::platform::RawInputEvent ev{};
    ev.type = lcl::platform::RawInputEventType::KeyboardKey;
    ev.key = key;
    ev.pressed = pressed;
    ev.modifiers = modifiers;
    ev.codepoint = codepoint;
    handleInput(ev);
}

void TerminalApp::handleKey(uint32_t keycode, bool pressed, uint8_t modifiers, char32_t codepoint) {
    handleKey(static_cast<lcl::platform::PhysicalKey>(keycode), pressed, modifiers, codepoint);
}

void TerminalApp::handleText(const std::string& text) {
    if (!m_initialized || text.empty()) return;
    m_lastInputTime = std::chrono::steady_clock::now();
    m_ptyManager.writeInput(text);
}



void TerminalApp::shutdown() {
    if (!m_initialized) return;
    std::cout << "[LCL App] Shutting down LCL Terminal App (Window ID: " << m_windowId << ")...\n";
    m_ptyManager.shutdown();
    m_initialized = false;
}

} // namespace lcl::apps
