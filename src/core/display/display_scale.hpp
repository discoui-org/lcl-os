#pragma once

#include <cstdint>

namespace lcl::core {

/**
 * UI DPI scale from boot args (lcl.scale=) or default 1.0.
 * Layout constants are logical (scale=1); use px() for framebuffer pixels.
 */
class DisplayScale {
public:
    static constexpr int kMenuBarHeight = 40;
    static constexpr int kTitleBarHeight = 32;
    static constexpr int kWindowPad = 12;
    static constexpr int kTrafficBtn = 12;
    static constexpr int kTrafficGap = 18;
    static constexpr int kBaseFontPx = 15;
    static constexpr int kDefaultWinW = 640;
    static constexpr int kDefaultWinH = 480;

    /** Parse /proc/cmdline (and optional LCL_SCALE env). Safe to call multiple times. */
    static void initialize();

    static float factor();
    static int px(int logical);
    static float pxF(float logical);

    static int menuBarHeight() { return px(kMenuBarHeight); }
    static int titleBarHeight() { return px(kTitleBarHeight); }
    static int windowPad() { return px(kWindowPad); }
    static int trafficBtn() { return px(kTrafficBtn); }
    static int trafficGap() { return px(kTrafficGap); }
    static float fontSize() { return pxF(static_cast<float>(kBaseFontPx)); }

private:
    static float& scaleRef();
    static bool& readyRef();
};

} // namespace lcl::core
