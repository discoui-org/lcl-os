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

#include "core/display/display_scale.hpp"
#include "core/ipc/lcl_protocol.hpp"
#include "core/session/session_client.hpp"
#include "core/shell/shell_state_client.hpp"
#include "core/shell/shell_state_model.hpp"
#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/backdrop_surface.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/image.hpp"
#include "lcl-ui/widgets/text.hpp"
#include "render/skia_canvas.hpp"

namespace {

constexpr uint32_t kMenuBarHeight = 32;
constexpr uint32_t kDockHeight = 88;
constexpr uint32_t kDockBottomInset = 4;
constexpr int kIconSize = 60;
constexpr int kIconGap = 12;
constexpr int kIconPadX = 14;

struct DockWindow { uint64_t sceneId{}; std::string title; std::string appId; bool focused{}; };
struct DockLayout { int x{}, y{}, width{}, height{}, first{}, count{}; };

struct DockView {
    lcl::ui::Container* root{nullptr};
    lcl::ui::Container* panel{nullptr};
    lcl::ui::BackdropSurface* backdrop{nullptr};
    lcl::ui::Container* innerBorder{nullptr};
    std::vector<lcl::ui::Widget*> dynamicChildren;
    uint32_t width{0};
    uint32_t height{0};
};

/** Shell-local projection: Dock groups scenes by application, never by raw window ID. */
class DockStateModel {
public:
    bool applySnapshot(const lcl::shell::ShellStateSnapshot& snapshot) {
        return m_state.applySnapshot(snapshot);
    }

    bool applyDelta(const lcl::shell::ShellStateDelta& delta) {
        return m_state.applyDelta(delta);
    }

    std::vector<DockWindow> items() const {
        std::vector<DockWindow> result;
        std::unordered_map<std::string, size_t> byAppId;
        // The clean profile pins Terminal. It becomes the running entry once
        // its first scene arrives, rather than being a parallel ad-hoc list.
        result.push_back({0, "LCL Terminal", "org.lcl.terminal", false});
        byAppId.emplace("org.lcl.terminal", 0);
        for (const auto& scene : m_state.snapshot().scenes) {
            if (scene.visibility == lcl::protocol::LCLSceneVisibility::Closing) continue;
            const std::string appId = scene.appId.empty() ? "unknown" : scene.appId;
            const bool focused = scene.sceneId == m_state.snapshot().activeSceneId;
            const auto existing = byAppId.find(appId);
            if (existing == byAppId.end()) {
                byAppId.emplace(appId, result.size());
                result.push_back({scene.sceneId, scene.title, appId, focused});
            } else {
                auto& item = result[existing->second];
                item.sceneId = scene.sceneId;
                item.title = scene.title;
                item.focused = item.focused || focused;
            }
        }
        return result;
    }

private:
    lcl::shell::ShellStateModel m_state;
};

std::string normalize(std::string value) {
    std::string out;
    for (unsigned char c : value) if (std::isalnum(c)) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

std::unordered_map<std::string, std::string> iconCatalog(const std::vector<lcl::session::CatalogEntry>& entries) {
    std::unordered_map<std::string, std::string> result;
    for (const auto& app : entries) {
        if (app.appId.empty() || app.icon.empty()) continue;
        result[app.appId] = app.icon;
        result[normalize(app.name)] = app.icon;
    }
    return result;
}

std::string iconFor(const DockWindow& window, const std::unordered_map<std::string, std::string>& catalog) {
    for (const auto& key : {window.appId, normalize(window.title)}) {
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

std::unique_ptr<lcl::ui::Container> makeDockView(DockView& view, uint32_t width, uint32_t height) {
    view = {};
    view.width = width;
    view.height = height;
    auto root = std::make_unique<lcl::ui::Container>();
    view.root = root.get();
    root->getYogaNode().setWidth(width); root->getYogaNode().setHeight(height);

    auto panel = std::make_unique<lcl::ui::Container>();
    view.panel = panel.get();
    panel->setBackgroundColor({17, 19, 23, 184});
    panel->setBorderColor({255, 255, 255, 68}); panel->setBorderWidth(1.0f);
    panel->setBorderRadius(26.0f); panel->setBorderRoundness(2.0f);

    // Match the terminal's translucent composition: paint the alpha tint on
    // the panel, while a separate transparent surface owns the glass effect.
    auto backdrop = std::make_unique<lcl::ui::BackdropSurface>();
    view.backdrop = backdrop.get();
    backdrop->setInteractive(false);
    backdrop->setBorderRadius(26.0f);
    backdrop->setBorderRoundness(2.0f);
    backdrop->addFilter(lcl::protocol::FilterType::Glass, 30.0f, 3.0f, 12.0f);
    panel->addChild(std::move(backdrop));

    auto innerBorder = std::make_unique<lcl::ui::Container>();
    view.innerBorder = innerBorder.get();
    innerBorder->setBorderColor({0, 0, 0, 34}); innerBorder->setBorderWidth(1.0f);
    innerBorder->setBorderRadius(24.0f); innerBorder->setBorderRoundness(2.0f);
    panel->addChild(std::move(innerBorder));

    root->addChild(std::move(panel));
    return root;
}

void updateDockView(DockView& view, const std::vector<DockWindow>& windows,
                    const std::unordered_map<std::string, std::string>& catalog) {
    if (!view.root || !view.panel || !view.backdrop || !view.innerBorder) return;
    for (auto* child : view.dynamicChildren) view.panel->removeChild(child);
    view.dynamicChildren.clear();

    const DockLayout layout = layoutDock(view.width, view.height, windows.size());
    absolute(*view.panel, layout.x, layout.y, layout.width, layout.height);
    absolute(*view.backdrop, 0, 0, layout.width, layout.height);
    absolute(*view.innerBorder, 2, 2, std::max(0, layout.width - 4), std::max(0, layout.height - 4));
    view.panel->setVisible(layout.count > 0);
    if (layout.count == 0) return;

    int iconX = kIconPadX;
    const int iconY = std::max(0, (layout.height - kIconSize) / 2);
    for (int i = 0; i < layout.count; ++i) {
        const auto& window = windows[static_cast<size_t>(layout.first + i)];
        const std::string path = iconFor(window, catalog);
        if (!path.empty()) {
            auto image = std::make_unique<lcl::ui::Image>(path);
            image->setFit(lcl::ui::ImageFit::Contain); image->setCornerRadius(14.0f);
            absolute(*image, iconX, iconY, kIconSize, kIconSize);
            view.dynamicChildren.push_back(image.get());
            view.panel->addChild(std::move(image));
        }
        if (window.focused) {
            auto indicator = std::make_unique<lcl::ui::Container>();
            indicator->setBackgroundColor({246, 248, 252, 238}); indicator->setBorderRadius(1.5f);
            absolute(*indicator, iconX + kIconSize / 2 - 10, iconY + kIconSize + 4, 20, 3);
            view.dynamicChildren.push_back(indicator.get());
            view.panel->addChild(std::move(indicator));
        }
        iconX += kIconSize + kIconGap;
    }
    view.panel->markDirty();
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
    auto wallpaper = std::make_unique<lcl::ui::WindowApp>(
        lcl::render::makeSkiaCanvas(), width, height, "LCL Wallpaper");
    wallpaper->setSurfaceId(1); wallpaper->setSystemSurfaceKind(lcl::protocol::LCLSystemSurfaceKind::Wallpaper);
    wallpaper->setAppId("org.lcl.desktop-shell");
    wallpaper->setInputEnabled(false);
    // This is a bootstrap buffer size only. The compositor's Wallpaper policy
    // assigns the surface to the actual output bounds before it is mapped.
    wallpaper->setInitialBounds(0, 0, width, height);
    auto wallpaperImage = std::make_unique<lcl::ui::Image>(wallpaperPath);
    wallpaperImage->setFit(lcl::ui::ImageFit::Fill);
    wallpaperImage->getYogaNode().setWidth(static_cast<float>(width));
    wallpaperImage->getYogaNode().setHeight(static_cast<float>(height));
    wallpaper->setRootWidget(std::move(wallpaperImage));
    if (!wallpaper->connectCompositor()) return 1;
    wallpaper->setDecorationMode(lcl::protocol::LCLDecorationMode::None);

    std::unique_ptr<lcl::ui::WindowApp> menu;
    std::unique_ptr<lcl::ui::WindowApp> dock;
    lcl::ui::Text* clock = nullptr;
    DockStateModel dockState;
    DockView dockView;
    std::vector<lcl::session::CatalogEntry> catalogEntries;
    std::string catalogError;
    lcl::session::SessionClient session;
    if (session.connect() && !session.requestCatalog(catalogEntries, catalogError)) {
        std::cerr << "[LCL Shell] Could not read session catalog: " << catalogError << "\n";
    }
    auto catalog = iconCatalog(catalogEntries);

    auto refreshDock = [&]() {
        if (!dock || !dockView.root) return;
        updateDockView(dockView, dockState.items(), catalog);
    };

    lcl::shell::ShellStateClient shellState;
    shellState.setOnSnapshot([&](const lcl::shell::ShellStateSnapshot& snapshot) {
        dockState.applySnapshot(snapshot);
        refreshDock();
    });
    shellState.setOnDelta([&](const lcl::shell::ShellStateDelta& delta) {
        dockState.applyDelta(delta);
        refreshDock();
    });
    if (!shellState.connect()) {
        std::cerr << "[LCL Shell] Could not subscribe to compositor shell state\n";
    }

    auto createPanels = [&]() -> bool {
        dock.reset(); menu.reset(); clock = nullptr; dockView = {};

        // Stage both shell surfaces completely before connecting either one.
        // The reserved work area is published only after both panels have made
        // their first real buffer commit, so menu setup cannot leave the shell
        // in a half-created state with no dock.
        menu = std::make_unique<lcl::ui::WindowApp>(
            lcl::render::makeSkiaCanvas(), width, kMenuBarHeight, "LCL MenuBar");
        menu->setSurfaceId(2); menu->setSystemSurfaceKind(lcl::protocol::LCLSystemSurfaceKind::MenuBar);
        menu->setAppId("org.lcl.desktop-shell");
        menu->setInputEnabled(false);
        menu->setInitialBounds(0, 0, width, kMenuBarHeight);
        menu->setRootWidget(makeMenuRoot(width, clock));
        menu->setOnResize([&](uint32_t resizedWidth, uint32_t) {
            width = resizedWidth;
            menu->setRootWidget(makeMenuRoot(resizedWidth, clock));
        });
        menu->setDecorationMode(lcl::protocol::LCLDecorationMode::None);

        dock = std::make_unique<lcl::ui::WindowApp>(
            lcl::render::makeSkiaCanvas(), width, kDockHeight, "LCL Dock");
        dock->setSurfaceId(3); dock->setSystemSurfaceKind(lcl::protocol::LCLSystemSurfaceKind::Dock);
        dock->setAppId("org.lcl.desktop-shell");
        dock->setInputEnabled(false);
        dock->setInitialBounds(0, static_cast<int32_t>(height > kDockHeight ? height - kDockHeight : 0), width, kDockHeight);
        auto dockRoot = makeDockView(dockView, width, kDockHeight);
        updateDockView(dockView, dockState.items(), catalog);
        dock->setRootWidget(std::move(dockRoot));
        dock->setOnResize([&](uint32_t resizedWidth, uint32_t resizedHeight) {
            width = resizedWidth;
            auto resizedRoot = makeDockView(dockView, resizedWidth, resizedHeight);
            updateDockView(dockView, dockState.items(), catalog);
            dock->setRootWidget(std::move(resizedRoot));
        });
        dock->setDecorationMode(lcl::protocol::LCLDecorationMode::None);
        dock->setWindowCornerRadius(26.0f);

        if (!dock->connectCompositor()) return false;
        if (!menu->connectCompositor()) return false;

        return true;
    };
    if (!createPanels()) return 1;

    std::string lastTime;
    auto nextShellReconnect = std::chrono::steady_clock::now();
    while (true) {
        const auto frameStart = std::chrono::steady_clock::now();
        const std::string now = timeText();
        if (clock && now != lastTime) { lastTime = now; clock->setText(now); }
        bool rendered = wallpaper->tick();
        if (!shellState.isConnected() && std::chrono::steady_clock::now() >= nextShellReconnect) {
            shellState.connect();
            nextShellReconnect = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
        shellState.poll();
        rendered = menu->tick() || rendered;
        rendered = dock->tick() || rendered;

        // This process drives three WindowApps itself, so it must provide the
        // same active-frame pacing as WindowApp::runEventLoop().  The old fixed
        // 16 ms delay capped all shell animations at roughly 60 Hz.
        if (rendered) {
            constexpr auto kActiveFramePeriod = std::chrono::microseconds(6900);
            const auto elapsed = std::chrono::steady_clock::now() - frameStart;
            if (elapsed < kActiveFramePeriod) {
                std::this_thread::sleep_for(kActiveFramePeriod - elapsed);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}
