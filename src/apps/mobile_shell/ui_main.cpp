#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "core/ipc/lcl_protocol.hpp"
#include "core/session/session_client.hpp"
#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/image.hpp"
#include "lcl-ui/widgets/scroll_view.hpp"
#include "lcl-ui/widgets/text.hpp"
#include "render/raster_canvas.hpp"

namespace {

constexpr float kLauncherPaddingX = 24.0f;
constexpr float kLauncherPaddingY = 32.0f;
constexpr float kTileWidth = 84.0f;
constexpr float kTileHeight = 92.0f;
constexpr float kIconSize = 60.0f;
constexpr float kIconRadius = 14.0f;
constexpr float kColumnGap = 18.0f;
constexpr float kRowGap = 22.0f;

void absolute(lcl::ui::Widget& widget, float x, float y, float width,
              float height) {
    auto& yoga = widget.getYogaNode();
    yoga.setPositionType(YGPositionTypeAbsolute);
    yoga.setPosition(YGEdgeLeft, x);
    yoga.setPosition(YGEdgeTop, y);
    yoga.setWidth(width);
    yoga.setHeight(height);
}

std::unique_ptr<lcl::ui::Widget> makeIcon(
    const lcl::session::CatalogEntry& app) {
    if (!app.icon.empty()) {
        auto image = std::make_unique<lcl::ui::Image>(app.icon);
        if (image->hasImage()) {
            image->setFit(lcl::ui::ImageFit::Contain);
            image->setCornerRadius(kIconRadius);
            image->getYogaNode().setWidth(kIconSize);
            image->getYogaNode().setHeight(kIconSize);
            return image;
        }
    }

    auto placeholder = std::make_unique<lcl::ui::Container>();
    placeholder->setBackgroundColor({30, 41, 59, 210});
    placeholder->setBorderColor({255, 255, 255, 58});
    placeholder->setBorderWidth(1.0f);
    placeholder->setBorderRadius(kIconRadius);
    placeholder->getYogaNode().setWidth(kIconSize);
    placeholder->getYogaNode().setHeight(kIconSize);
    return placeholder;
}

std::unique_ptr<lcl::ui::Container> makeLauncherTile(
    const lcl::session::CatalogEntry& app,
    lcl::session::SessionClient& session) {
    auto tile = std::make_unique<lcl::ui::Container>();
    tile->getYogaNode().setDirection(YGFlexDirectionColumn);
    tile->getYogaNode().setAlignItems(YGAlignCenter);
    tile->getYogaNode().setGap(YGGutterAll, 6.0f);
    tile->getYogaNode().setWidth(kTileWidth);
    tile->getYogaNode().setHeight(kTileHeight);

    auto icon = makeIcon(app);
    lcl::ui::Widget* iconView = icon.get();
    tile->addChild(std::move(icon));

    auto label = std::make_unique<lcl::ui::Text>(
        app.name.empty() ? app.appId : app.name);
    label->setFontSize(12.0f);
    label->setTextColor({255, 255, 255, 255});
    label->setTextAlign(lcl::ui::TextAlign::Center);
    label->getYogaNode().setWidth(kTileWidth);
    tile->addChild(std::move(label));

    const std::string appId = app.appId;
    tile->setOnClick([&session, appId, iconView]() {
        const auto bounds = iconView->getPresentationBounds();
        const float centerX = bounds.x + bounds.width * 0.5f;
        const float centerY = bounds.y + bounds.height * 0.5f;
        lcl::session::LaunchRequest request;
        request.target = appId;
        request.origin = {
            .valid = !bounds.isEmpty(),
            .x = centerX - kIconSize * 0.5f,
            .y = centerY - kIconSize * 0.5f,
            .width = kIconSize,
            .height = kIconSize,
            .cornerRadius = kIconRadius,
        };
        lcl::session::LaunchResponse response;
        std::string error;
        if (!session.launch(request, response, error)) {
            std::cerr << "[LCL Mobile Shell] Could not launch " << appId
                      << ": " << error << "\n";
        }
    });

    lcl::ui::InteractionStyle pressed;
    pressed.scale = 0.92f;
    pressed.opacity = 0.82f;
    tile->setInteractionStyle(lcl::ui::InteractionState::Pressed, pressed);
    return tile;
}

std::unique_ptr<lcl::ui::Container> makeHomeRoot(
    const std::string& wallpaperPath,
    uint32_t width,
    uint32_t height,
    const std::vector<lcl::session::CatalogEntry>& apps,
    lcl::session::SessionClient& session) {
    auto root = std::make_unique<lcl::ui::Container>();
    root->getYogaNode().setWidth(static_cast<float>(width));
    root->getYogaNode().setHeight(static_cast<float>(height));

    auto wallpaper = std::make_unique<lcl::ui::Image>(wallpaperPath);
    wallpaper->setFit(lcl::ui::ImageFit::Cover);
    absolute(*wallpaper, 0.0f, 0.0f,
             static_cast<float>(width), static_cast<float>(height));
    root->addChild(std::move(wallpaper));

    auto grid = std::make_unique<lcl::ui::Container>();
    grid->getYogaNode().setDirection(YGFlexDirectionRow);
    grid->getYogaNode().setFlexWrap(YGWrapWrap);
    grid->getYogaNode().setAlignItems(YGAlignFlexStart);
    grid->getYogaNode().setPadding(YGEdgeHorizontal, kLauncherPaddingX);
    grid->getYogaNode().setPadding(YGEdgeVertical, kLauncherPaddingY);
    grid->getYogaNode().setGap(YGGutterColumn, kColumnGap);
    grid->getYogaNode().setGap(YGGutterRow, kRowGap);
    grid->getYogaNode().setWidth(static_cast<float>(width));
    for (const auto& app : apps) {
        grid->addChild(makeLauncherTile(app, session));
    }

    auto launcher = std::make_unique<lcl::ui::ScrollView>();
    absolute(*launcher, 0.0f, 0.0f,
             static_cast<float>(width), static_cast<float>(height));
    launcher->setContent(std::move(grid));
    root->addChild(std::move(launcher));
    return root;
}

} // namespace

int main() {
    uint32_t width = 1280;
    uint32_t height = 800;
    const std::string wallpaperPath =
        std::filesystem::exists("/System/Library/Wallpapers/wallpaper.jpg")
            ? "/System/Library/Wallpapers/wallpaper.jpg"
            : "/usr/share/wallpaper.jpg";

    lcl::session::SessionClient session;
    std::vector<lcl::session::CatalogEntry> apps;
    std::string catalogError;
    if (!session.connect()) {
        std::cerr << "[LCL Mobile Shell] Could not connect to lcl-sessiond\n";
    } else if (!session.requestCatalog(apps, catalogError)) {
        std::cerr << "[LCL Mobile Shell] Could not read application catalog: "
                  << catalogError << "\n";
    }

    lcl::ui::WindowApp home(lcl::render::makeRasterCanvas(), width, height,
                            "LCL Mobile Home");
    home.setSurfaceId(1);
    home.setSystemSurfaceKind(lcl::protocol::LCLSystemSurfaceKind::HomeScreen);
    home.setAppId("org.lcl.mobile-shell");
    home.setInputEnabled(true);
    home.setInitialBounds(0, 0, width, height);
    home.setRootWidget(
        makeHomeRoot(wallpaperPath, width, height, apps, session));
    home.setOnResize([&](uint32_t resizedWidth, uint32_t resizedHeight) {
        width = resizedWidth;
        height = resizedHeight;
        home.setRootWidget(
            makeHomeRoot(wallpaperPath, width, height, apps, session));
    });
    home.setDecorationMode(lcl::protocol::LCLDecorationMode::None);

    if (!home.connectCompositor()) return 1;
    home.runEventLoop();
    return 0;
}
