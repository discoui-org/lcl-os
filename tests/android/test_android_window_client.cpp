#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "render/raster_canvas.hpp"

#include <cstdint>
#include <iostream>
#include <memory>

int main() {
    constexpr float kWidth = 360.0f;
    constexpr float kHeight = 240.0f;
    lcl::ui::WindowApp window(
        lcl::render::makeDisplayListCanvas(), kWidth, kHeight,
        "Android AHB Client Test");
    window.setAppId("org.lcl.test.ahb-client");
    window.setInitialBounds(120.0f, 180.0f, kWidth, kHeight);
    window.setDecorationMode(lcl::protocol::LCLDecorationMode::SSD);

    auto root = std::make_unique<lcl::ui::Container>();
    auto* rootView = root.get();
    root->setWidth(kWidth);
    root->setHeight(kHeight);
    root->setBackgroundColor({30, 90, 220, 255});
    window.setRootWidget(std::move(root));

    uint32_t ticks = 0;
    window.setOnFrame([&] {
        ++ticks;
        if ((ticks % 8u) == 0u) {
            const uint8_t phase = static_cast<uint8_t>((ticks * 3u) % 120u);
            rootView->setBackgroundColor({
                static_cast<uint8_t>(40u + phase),
                static_cast<uint8_t>(150u - phase / 2u),
                220,
                255,
            });
            rootView->invalidatePaint();
        }
        if (ticks == 180u) window.requestWindowClose();
    });

    if (!window.connectCompositor()) {
        std::cerr << "android_window_client_connect=no\n";
        return 1;
    }
    window.runEventLoop();
    std::cout << "android_window_client_connect=yes\n"
              << "android_window_client_ticks=" << ticks << "\n";
    return ticks >= 180u ? 0 : 2;
}
