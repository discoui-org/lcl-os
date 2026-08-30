#pragma once

#include <cstdint>

namespace lcl::platform {

/**
 * @brief Platform-independent physical key enum.
 *
 * Defines logical hardware key identities across all platform backends
 * (Desktop evdev, Android/Cuttlefish, etc.). Free of any Linux KEY_* or
 * Android AKEYCODE_* dependencies.
 */
enum class PhysicalKey : uint32_t {
    Unknown = 0,

    // Letters
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,

    // Digits (Top row)
    Digit1, Digit2, Digit3, Digit4, Digit5,
    Digit6, Digit7, Digit8, Digit9, Digit0,

    // Function keys
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,

    // Common Control / Spacing
    Enter,
    Escape,
    Backspace,
    Tab,
    Space,

    // Modifiers
    LeftShift,
    RightShift,
    LeftCtrl,
    RightCtrl,
    LeftAlt,
    RightAlt,
    LeftMeta,
    RightMeta,
    CapsLock,

    // Navigation & Editing
    ArrowUp,
    ArrowDown,
    ArrowLeft,
    ArrowRight,
    Home,
    End,
    PageUp,
    PageDown,
    Insert,
    Delete,

    // Symbols & Punctuation
    Minus,        // '-'
    Equal,        // '='
    LeftBracket,  // '['
    RightBracket, // ']'
    Backslash,    // '\'
    Semicolon,    // ';'
    Apostrophe,   // '''
    Grave,        // '`'
    Comma,        // ','
    Period,       // '.'
    Slash,        // '/'

    // Keypad
    KpEnter
};

// Platform-neutral Modifier bitmask flags
constexpr uint8_t kModShift    = 1 << 0; // 0x01
constexpr uint8_t kModCtrl     = 1 << 1; // 0x02
constexpr uint8_t kModAlt      = 1 << 2; // 0x04
constexpr uint8_t kModCapsLock = 1 << 3; // 0x08
constexpr uint8_t kModSuper    = 1 << 4; // 0x10

} // namespace lcl::platform
