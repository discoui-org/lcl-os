#pragma once

#include <cstdint>

/**
 * @file palette.hpp
 * @brief LCL OS Design System — Centralized Color Palette
 *
 * Base: Catppuccin Mocha (https://github.com/catppuccin/catppuccin)
 * Format: 0xAARRGGBB (ARGB32, premultiplied alpha=0xFF for opaque colors)
 */

namespace lcl::theme {

// ---------------------------------------------------------------------------
// Catppuccin Mocha — Base Palette
// ---------------------------------------------------------------------------
namespace Mocha {
    // Backgrounds
    inline constexpr uint32_t Base      = 0xFF1E1E2E; ///< Main background
    inline constexpr uint32_t Mantle    = 0xFF181825; ///< Darker background
    inline constexpr uint32_t Crust     = 0xFF11111B; ///< Darkest background

    // Surfaces
    inline constexpr uint32_t Surface0  = 0xFF313244; ///< Window body
    inline constexpr uint32_t Surface1  = 0xFF45475A; ///< Unfocused title bar
    inline constexpr uint32_t Surface2  = 0xFF585B70; ///< Subtle borders

    // Overlays
    inline constexpr uint32_t Overlay0  = 0xFF6C7086;
    inline constexpr uint32_t Overlay1  = 0xFF7F849C;
    inline constexpr uint32_t Overlay2  = 0xFF9399B2;

    // Text
    inline constexpr uint32_t Text      = 0xFFCDD6F4; ///< Primary text
    inline constexpr uint32_t Subtext0  = 0xFFA6ADC8; ///< Secondary text (status bar)
    inline constexpr uint32_t Subtext1  = 0xFFBAC2DE;

    // Accent Colors
    inline constexpr uint32_t Blue      = 0xFF89B4FA; ///< Focused title bar, logo
    inline constexpr uint32_t Lavender  = 0xFFB4BEFE;
    inline constexpr uint32_t Sapphire  = 0xFF74C7EC;
    inline constexpr uint32_t Sky       = 0xFF89DCEB;
    inline constexpr uint32_t Teal      = 0xFF94E2D5;
    inline constexpr uint32_t Green     = 0xFFA6E3A1; ///< Terminal output text
    inline constexpr uint32_t Yellow    = 0xFFF9E2AF;
    inline constexpr uint32_t Peach     = 0xFFFAB387;
    inline constexpr uint32_t Maroon    = 0xFFEBA0AC;
    inline constexpr uint32_t Red       = 0xFFEF4444; ///< Close button
    inline constexpr uint32_t Mauve     = 0xFFCBA6F7;
    inline constexpr uint32_t Pink      = 0xFFF38BA8;
    inline constexpr uint32_t Flamingo  = 0xFFF2CDCD;
    inline constexpr uint32_t Rosewater = 0xFFF5E0DC;
} // namespace Mocha

// ---------------------------------------------------------------------------
// LCL UI Semantic Tokens
// (Derived from Catppuccin Mocha — change theme here, not at callsites)
// ---------------------------------------------------------------------------
namespace UI {
    // Desktop / Wallpaper
    inline constexpr uint32_t Wallpaper         = 0xFF090D16; ///< Deep dark wallpaper

    // Shell / Taskbar
    inline constexpr uint32_t TaskbarBg         = Mocha::Base;
    inline constexpr uint32_t TaskbarBorder      = Mocha::Surface1;
    inline constexpr uint32_t TaskbarLogoBtn     = Mocha::Blue;
    inline constexpr uint32_t TaskbarLogoBtnText = Mocha::Base;       ///< Dark text on blue
    inline constexpr uint32_t TaskbarStatusText  = Mocha::Subtext0;

    // Window Chrome
    inline constexpr uint32_t WindowBorder       = 0xFF38BDF8;        ///< Sky-blue outline
    inline constexpr uint32_t WindowBodyBg       = 0xFF1E293B;        ///< Dark content area
    inline constexpr uint32_t WindowTitleFocused = Mocha::Blue;       ///< Focused header
    inline constexpr uint32_t WindowTitleBlurred = Mocha::Surface1;   ///< Unfocused header
    inline constexpr uint32_t WindowTitleText    = 0xFFFFFFFF;

    // Traffic-light buttons
    inline constexpr uint32_t BtnClose          = Mocha::Red;
    inline constexpr uint32_t BtnMinimize       = 0xFFF59E0B;         ///< Amber/Yellow
    inline constexpr uint32_t BtnMaximize       = 0xFF10B981;         ///< Emerald Green

    // Terminal
    inline constexpr uint32_t TerminalText      = Mocha::Green;
    inline constexpr uint32_t CursorBlock       = 0xFFE2E8F0;         ///< Light slate cursor
    inline constexpr uint32_t CursorText        = 0xFF0F172A;         ///< Inverted dark ink
} // namespace UI

} // namespace lcl::theme
