#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/app/app_bundle_parser.hpp"
#include "core/display/display_scale.hpp"
#include "core/ipc/ipc_manager.hpp"
#include "core/ipc/lcl_protocol.hpp"
#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/backdrop_surface.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/image.hpp"
#include "lcl-ui/widgets/text.hpp"

namespace {

constexpr uint32_t kMenuBarHeight = 32;
constexpr uint32_t kDockHeight = 88;
constexpr uint32_t kDockBottomInset = 4;
constexpr int kIconSize = 60;
constexpr int kIconGap = 12;
constexpr int kIconPadX = 14;

struct DockWindow { uint32_t id{}; std::string title; std::string appId; bool focused{}; };
struct DockLayout { int x{}, y{}, width{}, height{}, first{}, count{}; };

std::string normalize(std::string value) {
    std::string out;
    for (unsigned char c : value) if (std::isalnum(c)) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

std::unordered_map<std::string, std::string> iconCatalog() {
    std::unordered_map<std::string, std::string> result;
    for (const auto& dir : {"/home/user/Applications", "/Applications"}) {
        for (const auto& app : lcl::core::AppBundleParser::scanDirectory(dir)) {
            if (!app.valid || app.icon.empty()) continue;
            std::filesystem::path path(app.icon);
            if (path.is_relative()) path = std::filesystem::path(app.bundlePath) / path;
            if (!std::filesystem::exists(path)) continue;
            result[normalize(app.name)] = path.string();
            result[normalize(std::filesystem::path(app.bundlePath).stem().string())] = path.string();
        }
    }
    return result;
}

std::string iconFor(const DockWindow& window, const std::unordered_map<std::string, std::string>& catalog) {
    for (const auto& key : {normalize(window.appId), normalize(window.title)}) {
        if (auto it = catalog.find(key); it != catalog.end()) return it->second;
    }
    return {};
}

DockLayout layoutDock(uint32_t width, uint32_t height, size_t windows) {
    DockLayout out{};
    out.height = std::min<int>(static_cast<int>(height - std::min(height, kDockBottomInset)), kDockHeight - kDockBottomInset);
    out.y = std::max(0, static_cast<int>(height) - out.height - static_cast<int>(kDockBottomInset));
    if (windows == 0 || out.height <= 0) return out;
    const int maxIcons = std::max(1, (static_cast<int>(width) - kIconPadX * 2 + kIconGap) / (kIconSize + kIconGap));
    out.count = std::min<int>(static_cast<int>(windows), maxIcons);
    out.first = std::max(0, static_cast<int>(windows) - out.count);
    out.width = out.count * kIconSize + std::max(0, out.count - 1) * kIconGap + kIconPadX * 2;
    out.x = std::max(0, (static_cast<int>(width) - out.width) / 2);
    return out;
}

std::string timeText() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char out[48];
    std::strftime(out, sizeof(out), "%a %b %e %I:%M %p", &tm);
    return out;
}

void absolute(lcl::ui::Widget& widget, float x, float y, float width, float height) {
    auto& yoga = widget.getYogaNode();
    yoga.setPositionType(YGPositionTypeAbsolute);
    yoga.setPosition(YGEdgeLeft, x);
    yoga.setPosition(YGEdgeTop, y);
    yoga.setWidth(width);
    yoga.setHeight(height);
}

std::unique_ptr<lcl::ui::Container> makeMenuRoot(uint32_t width, lcl::ui::Text*& clock) {
    auto root = std::make_unique<lcl::ui::Container>();
    root->getYogaNode().setWidth(width); root->getYogaNode().setHeight(kMenuBarHeight);
    auto glass = std::make_unique<lcl::ui::BackdropSurface>();
    glass->setBackgroundColor({15, 23, 42, 102});
    glass->addFilter(lcl::protocol::FilterType::Blur, 15.0f);
    glass->addFilter(lcl::protocol::FilterType::Saturation, 1.4f);
    glass->addFilter(lcl::protocol::FilterType::Brightness, 1.1f);
    glass->getYogaNode().setDirection(YGFlexDirectionRow);
    glass->getYogaNode().setJustifyContent(YGJustifyFlexEnd);
    glass->getYogaNode().setAlignItems(YGAlignCenter);
    glass->getYogaNode().setPadding(YGEdgeRight, 20.0f);
    absolute(*glass, 0, 0, width, kMenuBarHeight);

    auto text = std::make_unique<lcl::ui::Text>(timeText());
    clock = text.get();
    text->setFontSize(14.0f); text->setTextColor({241, 245, 249, 255});
    // Fill the logical row so TextAlign::End uses the renderer's measured
    // glyph width, while the parent keeps the label vertically centred.
    text->getYogaNode().setFlexGrow(1.0f);
    text->setTextAlign(lcl::ui::TextAlign::End);
    glass->addChild(std::move(text));

    auto border = std::make_unique<lcl::ui::Container>();
    border->setBackgroundColor({30, 41, 59, 153});
    absolute(*border, 0, kMenuBarHeight - 1, width, 1);
    glass->addChild(std::move(border));
    root->addChild(std::move(glass));
    return root;
}

std::unique_ptr<lcl::ui::Container> makeDockRoot(uint32_t width, uint32_t height,
                                                   const std::vector<DockWindow>& windows,
                                                   const std::unordered_map<std::string, std::string>& catalog) {
    auto root = std::make_unique<lcl::ui::Container>();
    root->getYogaNode().setWidth(width); root->getYogaNode().setHeight(height);
    const DockLayout layout = layoutDock(width, height, windows.size());
    if (layout.count == 0) return root;

    auto panel = std::make_unique<lcl::ui::BackdropSurface>();
    panel->setBackgroundColor({18, 24, 34, 80});
    panel->setBorderColor({255, 255, 255, 68}); panel->setBorderWidth(1.0f);
    panel->setBorderRadius(26.0f); panel->setGlass(30.0f, 3.0f, 12.0f);
    panel->addFilter(lcl::protocol::FilterType::Blur, 3.5f);
    panel->addFilter(lcl::protocol::FilterType::Saturation, 1.5f);
    panel->addFilter(lcl::protocol::FilterType::Contrast, 0.9f);
    absolute(*panel, layout.x, layout.y, layout.width, layout.height);

    auto innerBorder = std::make_unique<lcl::ui::Container>();
    innerBorder->setBorderColor({0, 0, 0, 34}); innerBorder->setBorderWidth(1.0f); innerBorder->setBorderRadius(24.0f);
    absolute(*innerBorder, 2, 2, layout.width - 4, layout.height - 4);
    panel->addChild(std::move(innerBorder));

    int iconX = kIconPadX;
    const int iconY = std::max(0, (layout.height - kIconSize) / 2);
    for (int i = 0; i < layout.count; ++i) {
        const auto& window = windows[static_cast<size_t>(layout.first + i)];
        const std::string path = iconFor(window, catalog);
        if (!path.empty()) {
            auto image = std::make_unique<lcl::ui::Image>(path);
            image->setFit(lcl::ui::ImageFit::Contain); image->setCornerRadius(14.0f);
            absolute(*image, iconX, iconY, kIconSize, kIconSize);
            panel->addChild(std::move(image));
        }
        if (window.focused) {
            auto indicator = std::make_unique<lcl::ui::Container>();
            indicator->setBackgroundColor({246, 248, 252, 238}); indicator->setBorderRadius(1.5f);
            absolute(*indicator, iconX + kIconSize / 2 - 10, iconY + kIconSize + 4, 20, 3);
            panel->addChild(std::move(indicator));
        }
        iconX += kIconSize + kIconGap;
    }
    root->addChild(std::move(panel));
    return root;
}

std::pair<uint32_t, uint32_t> displayPixelsFromCmdline() {
    uint32_t width = 1280, height = 800;
    std::ifstream cmdline("/proc/cmdline");
    std::string token;
    while (cmdline >> token) {
        if (token.rfind("lcl.width=", 0) == 0) width = std::max(1, std::stoi(token.substr(10)));
        if (token.rfind("lcl.height=", 0) == 0) height = std::max(1, std::stoi(token.substr(11)));
    }
    return {width, height};
}

} // namespace

int main() {
    lcl::core::DisplayScale::initialize();
    const float scale = lcl::core::DisplayScale::factor();
    const auto [physicalW, physicalH] = displayPixelsFromCmdline();
    uint32_t width = std::max(1u, static_cast<uint32_t>(std::lround(physicalW / scale)));
    uint32_t height = std::max(1u, static_cast<uint32_t>(std::lround(physicalH / scale)));

    const std::string wallpaperPath = std::filesystem::exists("/usr/share/wallpapers/wallpaper.jpg")
        ? "/usr/share/wallpapers/wallpaper.jpg" : "/usr/share/wallpaper.jpg";
    auto wallpaper = std::make_unique<lcl::ui::WindowApp>(width, height, "LCL Wallpaper");
    wallpaper->setSurfaceId(1); wallpaper->setRole(lcl::protocol::LCLRole::DesktopWallpaper);
    wallpaper->setInputEnabled(false);
    wallpaper->setInitialBounds(0, 0, width, height);
    auto wallpaperImage = std::make_unique<lcl::ui::Image>(wallpaperPath);
    wallpaperImage->setFit(lcl::ui::ImageFit::Fill);
    wallpaperImage->getYogaNode().setWidth(static_cast<float>(width));
    wallpaperImage->getYogaNode().setHeight(static_cast<float>(height));
    wallpaper->setRootWidget(std::move(wallpaperImage));
    if (!wallpaper->connectCompositor()) return 1;
    wallpaper->setDecorationMode(lcl::protocol::LCLDecorationMode::None);
    wallpaper->setWindowLayer(lcl::protocol::LCLWindowLayer::Bottom, true);

    std::unique_ptr<lcl::ui::WindowApp> menu;
    std::unique_ptr<lcl::ui::WindowApp> dock;
    lcl::ui::Text* clock = nullptr;
    std::vector<DockWindow> windows;
    auto catalog = iconCatalog();
    auto createPanels = [&]() -> bool {
        dock.reset(); menu.reset(); clock = nullptr;
        menu = std::make_unique<lcl::ui::WindowApp>(width, kMenuBarHeight, "LCL MenuBar");
        menu->setSurfaceId(2); menu->setRole(lcl::protocol::LCLRole::ShellPanel);
        menu->setInputEnabled(false);
        menu->setInitialBounds(0, 0, width, kMenuBarHeight);
        menu->setRootWidget(makeMenuRoot(width, clock));
        if (!menu->connectCompositor()) return false;
        menu->setDecorationMode(lcl::protocol::LCLDecorationMode::None);
        menu->setWindowLayer(lcl::protocol::LCLWindowLayer::TopMost, true);
        menu->setReservedZone(kMenuBarHeight, kDockHeight);

        dock = std::make_unique<lcl::ui::WindowApp>(width, kDockHeight, "LCL Dock");
        dock->setSurfaceId(3); dock->setRole(lcl::protocol::LCLRole::ShellPanel);
        dock->setInputEnabled(false);
        dock->setInitialBounds(0, static_cast<int32_t>(height > kDockHeight ? height - kDockHeight : 0), width, kDockHeight);
        dock->setRootWidget(makeDockRoot(width, kDockHeight, windows, catalog));
        dock->setOnIpcMessage([&](const lcl::protocol::LCLHeader& message, const std::vector<uint8_t>& data) {
            if (message.opcode != lcl::protocol::LCLOpcode::WindowListUpdate || data.size() < sizeof(lcl::protocol::LCLMsgWindowListHeader)) return;
            const auto* list = reinterpret_cast<const lcl::protocol::LCLMsgWindowListHeader*>(data.data());
            const size_t need = sizeof(*list) + static_cast<size_t>(list->windowCount) * sizeof(lcl::protocol::LCLMsgWindowListEntry);
            if (data.size() < need) return;
            const auto* entries = reinterpret_cast<const lcl::protocol::LCLMsgWindowListEntry*>(data.data() + sizeof(*list));
            windows.clear(); windows.reserve(list->windowCount);
            for (uint32_t i = 0; i < list->windowCount; ++i) windows.push_back({entries[i].windowId, entries[i].title, entries[i].appId, entries[i].isFocused != 0});
            catalog = iconCatalog();
            dock->setRootWidget(makeDockRoot(width, kDockHeight, windows, catalog));
        });
        if (!dock->connectCompositor()) return false;
        dock->setDecorationMode(lcl::protocol::LCLDecorationMode::None);
        dock->setWindowLayer(lcl::protocol::LCLWindowLayer::TopMost, true);
        dock->setWindowCornerRadius(26.0f);
        return true;
    };
    if (!createPanels()) return 1;

    uint32_t requestedWidth = width, requestedHeight = height;
    wallpaper->setOnIpcMessage([&](const lcl::protocol::LCLHeader& message, const std::vector<uint8_t>& data) {
        if (message.opcode != lcl::protocol::LCLOpcode::ConfigureBounds ||
            data.size() < offsetof(lcl::protocol::LCLMsgConfigureBounds, bufferScale)) return;
        const auto* cfg = reinterpret_cast<const lcl::protocol::LCLMsgConfigureBounds*>(data.data());
        if (cfg->surfaceId == 1 && cfg->width > 0 && cfg->height > 0) { requestedWidth = cfg->width; requestedHeight = cfg->height; }
    });

    std::string lastTime;
    while (true) {
        const std::string now = timeText();
        if (clock && now != lastTime) { lastTime = now; clock->setText(now); }
        wallpaper->tick();
        if (requestedWidth != width || requestedHeight != height) {
            width = requestedWidth; height = requestedHeight;
            if (!createPanels()) return 1;
            continue;
        }
        menu->tick();
        dock->tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}
