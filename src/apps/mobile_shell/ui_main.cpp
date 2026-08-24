#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/ipc/lcl_protocol.hpp"
#include "core/session/session_client.hpp"
#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/image.hpp"
#include "lcl-ui/widgets/scroll_view.hpp"
#include "lcl-ui/widgets/text.hpp"
#include "lcl-theme/theme.hpp"
#include "render/raster_canvas.hpp"

namespace {

constexpr float kLauncherPaddingX = 24.0f;
constexpr float kLauncherPaddingY = 32.0f;
constexpr float kTileWidth = 84.0f;
constexpr float kTileHeight = 92.0f;
constexpr float kIconSize = lcl::theme::mobile::kAppIconSize;
constexpr uint32_t kIconPixelSize =
    static_cast<uint32_t>(lcl::theme::mobile::kAppIconSize);
constexpr float kIconRadius = lcl::theme::mobile::kAppIconCornerRadius;
constexpr float kIconRoundness =
    lcl::theme::mobile::kAppIconCornerRoundness;
constexpr float kColumnGap = 18.0f;
constexpr float kRowGap = 22.0f;
constexpr uint64_t kLaunchTokenMask = (uint64_t{1} << 63) - 1;

struct LauncherIcon {
    std::unique_ptr<lcl::ui::Widget> widget;
    std::vector<uint32_t> pixels;
};

class LauncherState {
public:
    void beginRootRebuild() { m_iconViews.clear(); }

    void bindIcon(const std::string& appId, lcl::ui::Widget& icon) {
        m_iconViews[appId] = &icon;
        icon.setOpacity(m_activeLaunchTokens.contains(appId) ? 0.0f : 1.0f);
    }

    void hide(const std::string& appId, uint64_t launchToken) {
        m_activeLaunchTokens[appId] = launchToken;
        if (const auto found = m_iconViews.find(appId);
            found != m_iconViews.end()) {
            // Opacity affects paint only. Keep the icon visible in the widget
            // tree so pointer hit-testing can still bubble to its launcher tile.
            found->second->setOpacity(0.0f);
        }
    }

    void reveal(const std::string& appId, uint64_t launchToken) {
        const auto active = m_activeLaunchTokens.find(appId);
        if (active == m_activeLaunchTokens.end() ||
            active->second != launchToken) return;
        m_activeLaunchTokens.erase(active);
        if (const auto found = m_iconViews.find(appId);
            found != m_iconViews.end()) {
            found->second->setOpacity(1.0f);
        }
    }

private:
    std::unordered_map<std::string, lcl::ui::Widget*> m_iconViews;
    std::unordered_map<std::string, uint64_t> m_activeLaunchTokens;
};

std::vector<uint32_t> makeContainedIconSnapshot(
        const lcl::ui::ImageData& source) {
    std::vector<uint32_t> result(
        static_cast<size_t>(kIconPixelSize) * kIconPixelSize, 0);
    const float fitScale = std::min(
        static_cast<float>(kIconPixelSize) / source.width,
        static_cast<float>(kIconPixelSize) / source.height);
    const uint32_t width = std::max(
        1u, std::min(kIconPixelSize,
            static_cast<uint32_t>(std::lround(source.width * fitScale))));
    const uint32_t height = std::max(
        1u, std::min(kIconPixelSize,
            static_cast<uint32_t>(std::lround(source.height * fitScale))));
    const auto resized = lcl::ui::ImageLoader::resizeBilinear(
        source, width, height);
    const uint32_t offsetX = (kIconPixelSize - width) / 2;
    const uint32_t offsetY = (kIconPixelSize - height) / 2;
    for (uint32_t y = 0; y < height; ++y) {
        std::copy_n(
            resized.pixels.data() + static_cast<size_t>(y) * width,
            width,
            result.data() + static_cast<size_t>(offsetY + y) *
                kIconPixelSize + offsetX);
    }
    return result;
}

void absolute(lcl::ui::Widget& widget, float x, float y, float width,
              float height) {
    auto& yoga = widget.getYogaNode();
    yoga.setPositionType(YGPositionTypeAbsolute);
    yoga.setPosition(YGEdgeLeft, x);
    yoga.setPosition(YGEdgeTop, y);
    yoga.setWidth(width);
    yoga.setHeight(height);
}

LauncherIcon makeIcon(
    const lcl::session::CatalogEntry& app) {
    if (!app.icon.empty()) {
        const auto source = lcl::ui::ImageLoader::loadSharedArgb32(app.icon);
        auto image = std::make_unique<lcl::ui::Image>(app.icon);
        if (image->hasImage() && source && source->isValid()) {
            image->setFit(lcl::ui::ImageFit::Contain);
            image->setCornerRadius(kIconRadius);
            image->setCornerRoundness(kIconRoundness);
            image->getYogaNode().setWidth(kIconSize);
            image->getYogaNode().setHeight(kIconSize);
            return {std::move(image), makeContainedIconSnapshot(*source)};
        }
    }

    auto placeholder = std::make_unique<lcl::ui::Container>();
    placeholder->setBackgroundColor({30, 41, 59, 210});
    placeholder->setBorderColor({255, 255, 255, 58});
    placeholder->setBorderWidth(1.0f);
    placeholder->setBorderRadius(kIconRadius);
    placeholder->setBorderRoundness(kIconRoundness);
    placeholder->getYogaNode().setWidth(kIconSize);
    placeholder->getYogaNode().setHeight(kIconSize);
    std::vector<uint32_t> pixels(
        static_cast<size_t>(kIconPixelSize) * kIconPixelSize,
        0xD21E293Bu);
    for (uint32_t index = 0; index < kIconPixelSize; ++index) {
        pixels[index] = 0xDC5B6473u;
        pixels[static_cast<size_t>(kIconPixelSize - 1) * kIconPixelSize +
               index] = 0xDC5B6473u;
        pixels[static_cast<size_t>(index) * kIconPixelSize] = 0xDC5B6473u;
        pixels[static_cast<size_t>(index) * kIconPixelSize +
               kIconPixelSize - 1] = 0xDC5B6473u;
    }
    return {std::move(placeholder), std::move(pixels)};
}

std::unique_ptr<lcl::ui::Container> makeLauncherTile(
    const lcl::session::CatalogEntry& app,
    lcl::session::SessionClient& session,
    lcl::ui::WindowApp& home,
    LauncherState& launcherState) {
    auto tile = std::make_unique<lcl::ui::Container>();
    tile->getYogaNode().setDirection(YGFlexDirectionColumn);
    tile->getYogaNode().setAlignItems(YGAlignCenter);
    tile->getYogaNode().setGap(YGGutterAll, 6.0f);
    tile->getYogaNode().setWidth(kTileWidth);
    tile->getYogaNode().setHeight(kTileHeight);

    auto icon = makeIcon(app);
    lcl::ui::Widget* iconView = icon.widget.get();
    auto iconPixels = std::move(icon.pixels);
    launcherState.bindIcon(app.appId, *iconView);
    tile->addChild(std::move(icon.widget));

    auto label = std::make_unique<lcl::ui::Text>(
        app.name.empty() ? app.appId : app.name);
    label->setFontSize(12.0f);
    label->setTextColor({255, 255, 255, 255});
    label->setTextAlign(lcl::ui::TextAlign::Center);
    label->getYogaNode().setWidth(kTileWidth);
    tile->addChild(std::move(label));

    const std::string appId = app.appId;
    tile->setOnClick([&session, &home, &launcherState, appId, iconView,
                      iconPixels = std::move(iconPixels)]() {
        static uint64_t nextLaunchToken = [] {
            const auto seed = static_cast<uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count()) &
                kLaunchTokenMask;
            return seed == 0 ? uint64_t{1} : seed;
        }();
        const auto bounds = iconView->getPresentationBounds();
        if (bounds.isEmpty()) {
            std::cerr << "[LCL Mobile Shell] Icon has no launch geometry for "
                      << appId << "\n";
            return;
        }
        const float centerX = bounds.x + bounds.width * 0.5f;
        const float centerY = bounds.y + bounds.height * 0.5f;
        const lcl::graphics::RectF origin{
            centerX - kIconSize * 0.5f,
            centerY - kIconSize * 0.5f,
            kIconSize,
            kIconSize,
        };
        uint64_t launchToken = nextLaunchToken++ & kLaunchTokenMask;
        if (launchToken == 0) {
            launchToken = nextLaunchToken++ & kLaunchTokenMask;
        }
        if (!home.beginLaunchPlaceholder(
                launchToken, appId, origin, kIconRadius,
                kIconPixelSize, kIconPixelSize, iconPixels)) {
            std::cerr << "[LCL Mobile Shell] Could not begin launch visual for "
                      << appId << "\n";
            return;
        }
        launcherState.hide(appId, launchToken);

        lcl::session::LaunchRequest request;
        request.target = appId;
        request.singleInstance = true;
        request.launchToken = launchToken;
        request.origin = {
            .valid = !bounds.isEmpty(),
            .x = origin.x,
            .y = origin.y,
            .width = kIconSize,
            .height = kIconSize,
            .cornerRadius = kIconRadius,
        };
        lcl::session::LaunchResponse response;
        std::string error;
        if (!session.launch(request, response, error)) {
            home.cancelLaunchPlaceholder(launchToken);
            launcherState.reveal(appId, launchToken);
            std::cerr << "[LCL Mobile Shell] Could not launch " << appId
                      << ": " << error << "\n";
            return;
        }
        if (!home.resolveLaunchPlaceholder(
                launchToken, response.instanceId, response.reused)) {
            home.cancelLaunchPlaceholder(launchToken);
            launcherState.reveal(appId, launchToken);
            std::cerr << "[LCL Mobile Shell] Could not resolve launch visual for "
                      << appId << "\n";
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
    lcl::session::SessionClient& session,
    lcl::ui::WindowApp& home,
    LauncherState& launcherState) {
    launcherState.beginRootRebuild();
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
        grid->addChild(makeLauncherTile(
            app, session, home, launcherState));
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
    LauncherState launcherState;
    home.setSurfaceId(1);
    home.setSystemSurfaceKind(lcl::protocol::LCLSystemSurfaceKind::HomeScreen);
    home.setAppId("org.lcl.mobile-shell");
    home.setInputEnabled(true);
    home.setInitialBounds(0, 0, width, height);
    home.setRootWidget(
        makeHomeRoot(wallpaperPath, width, height, apps, session, home,
                     launcherState));
    home.setOnIpcMessage([&launcherState](
            const lcl::protocol::LCLHeader& header,
            const std::vector<uint8_t>& payload) {
        if (header.opcode !=
                lcl::protocol::LCLOpcode::LaunchIconVisibility ||
            payload.size() !=
                sizeof(lcl::protocol::LCLMsgLaunchIconVisibility)) return;
        const auto* message = reinterpret_cast<const
            lcl::protocol::LCLMsgLaunchIconVisibility*>(payload.data());
        if (message->visible != 0) {
            launcherState.reveal(message->appId, message->launchToken);
        }
    });
    home.setOnResize([&](uint32_t resizedWidth, uint32_t resizedHeight) {
        width = resizedWidth;
        height = resizedHeight;
        home.setRootWidget(
            makeHomeRoot(wallpaperPath, width, height, apps, session, home,
                         launcherState));
    });
    home.setDecorationMode(lcl::protocol::LCLDecorationMode::None);

    if (!home.connectCompositor()) return 1;
    home.runEventLoop();
    return 0;
}
