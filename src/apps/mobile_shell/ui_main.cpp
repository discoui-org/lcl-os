#include <filesystem>
#include <memory>
#include <string>

#include "core/ipc/lcl_protocol.hpp"
#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/image.hpp"
#include "render/raster_canvas.hpp"

namespace {

std::unique_ptr<lcl::ui::Image> makeHomeRoot(const std::string& wallpaperPath,
                                             uint32_t width,
                                             uint32_t height) {
    auto wallpaper = std::make_unique<lcl::ui::Image>(wallpaperPath);
    wallpaper->setFit(lcl::ui::ImageFit::Cover);
    wallpaper->getYogaNode().setWidth(static_cast<float>(width));
    wallpaper->getYogaNode().setHeight(static_cast<float>(height));
    return wallpaper;
}

} // namespace

int main() {
    uint32_t width = 1280;
    uint32_t height = 800;
    const std::string wallpaperPath =
        std::filesystem::exists("/System/Library/Wallpapers/wallpaper.jpg")
            ? "/System/Library/Wallpapers/wallpaper.jpg"
            : "/usr/share/wallpaper.jpg";

    lcl::ui::WindowApp home(lcl::render::makeRasterCanvas(), width, height,
                            "LCL Mobile Home");
    home.setSurfaceId(1);
    home.setSystemSurfaceKind(lcl::protocol::LCLSystemSurfaceKind::Wallpaper);
    home.setAppId("org.lcl.mobile-shell");
    home.setInputEnabled(false);
    home.setInitialBounds(0, 0, width, height);
    home.setRootWidget(makeHomeRoot(wallpaperPath, width, height));
    home.setOnResize([&](uint32_t resizedWidth, uint32_t resizedHeight) {
        width = resizedWidth;
        height = resizedHeight;
        home.setRootWidget(makeHomeRoot(wallpaperPath, width, height));
    });
    home.setDecorationMode(lcl::protocol::LCLDecorationMode::None);

    if (!home.connectCompositor()) return 1;
    home.runEventLoop();
    return 0;
}
