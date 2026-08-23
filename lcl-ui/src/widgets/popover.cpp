#include "lcl-ui/widgets/popover.hpp"

#include "lcl-graphics/canvas.hpp"
#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/focus_scope.hpp"
#include "lcl-ui/widgets/widget.hpp"

#include <atomic>
#include <cmath>
#include <utility>

namespace lcl::ui {
namespace {

std::atomic<uint32_t> g_nextPopoverSurfaceId{0x40000000u};

uint32_t allocatePopupSurfaceId(uint32_t parentSurfaceId) noexcept {
    uint32_t candidate = g_nextPopoverSurfaceId.fetch_add(1, std::memory_order_relaxed);
    if (candidate == 0 || candidate == parentSurfaceId) {
        candidate = g_nextPopoverSurfaceId.fetch_add(1, std::memory_order_relaxed);
    }
    return candidate == 0 ? 1 : candidate;
}

std::unique_ptr<FocusScope> makePanel(std::unique_ptr<Widget> content,
                                      const graphics::RectF& geometry) {
    auto panel = std::make_unique<FocusScope>();
    panel->setWidth(geometry.width);
    panel->setHeight(geometry.height);
    panel->getYogaNode().setDirection(YGFlexDirectionColumn);
    panel->useThemeStyle(lcl::theme::WidgetStyleRole::Popover);
    if (content) {
        panel->addChild(std::move(content));
    }
    return panel;
}

} // namespace

struct Popover::ActiveState {
    WindowApp* window{nullptr};
    std::weak_ptr<uint8_t> windowLifetime;
    TransientHandle transientHandle{0};
    HostedSurfaceHandle hostedSurfaceHandle{0};
    PopoverPresentation presentation{PopoverPresentation::PopupSurface};
    Widget* restoreTarget{nullptr};
    std::weak_ptr<uint8_t> restoreTargetLifetime;
    WindowApp* popupWindow{nullptr};
    std::weak_ptr<uint8_t> popupWindowLifetime;
    bool focusHandedOff{false};
    bool focusRestored{false};
};

Popover::Popover(WindowApp& window, PopupCanvasFactory popupCanvasFactory,
                 PopupConnector popupConnector)
    : m_window(window), m_windowLifetime(window.getLifetimeToken()),
      m_popupCanvasFactory(std::move(popupCanvasFactory)),
      m_popupConnector(std::move(popupConnector)) {}

Popover::~Popover() {
    closeActive();
}

graphics::RectF Popover::placeBelowLeft(const graphics::RectF& anchorRect,
                             float width, float height) noexcept {
    return {anchorRect.x, anchorRect.y + anchorRect.height, width, height};
}

PopoverOpenResult Popover::show(Widget& anchor,
                                std::unique_ptr<Widget> content,
                                PopoverOptions options) {
    return showImpl(anchor.getAbsoluteBounds(), &anchor,
                    anchor.getLifetimeToken(), true, std::move(content),
                    std::move(options));
}

PopoverOpenResult Popover::show(const graphics::RectF& anchorRect,
                                std::unique_ptr<Widget> content,
                                PopoverOptions options) {
    return showImpl(anchorRect, nullptr, {}, false, std::move(content),
                    std::move(options));
}

PopoverOpenResult Popover::showImpl(
        const graphics::RectF& anchorRect, Widget* restoreTarget,
        std::weak_ptr<uint8_t> ownerLifetime, bool trackOwnerLifetime,
        std::unique_ptr<Widget> content, PopoverOptions options) {
    closeActive();
    if (m_windowLifetime.expired() || !content ||
        !std::isfinite(options.width) || !std::isfinite(options.height) ||
        !std::isfinite(anchorRect.x) || !std::isfinite(anchorRect.y) ||
        !std::isfinite(anchorRect.width) || !std::isfinite(anchorRect.height) ||
        options.width <= 0.0f || options.height <= 0.0f ||
        anchorRect.width < 0.0f || anchorRect.height < 0.0f ||
        (trackOwnerLifetime && ownerLifetime.expired())) {
        return {};
    }

    const graphics::RectF geometry = placeBelowLeft(anchorRect, options.width, options.height);
    auto state = std::make_shared<ActiveState>();
    state->window = &m_window;
    state->windowLifetime = m_windowLifetime;
    state->presentation = PopoverPresentation::PopupSurface;
    state->restoreTarget = restoreTarget;
    state->restoreTargetLifetime = ownerLifetime;

    TransientOptions transientOptions{};
    transientOptions.dismissOnOutsidePointer = options.dismissOnOutsidePointer;
    transientOptions.trackOwnerLifetime = trackOwnerLifetime;
    transientOptions.ownerLifetime = ownerLifetime;
    transientOptions.onDismiss = [weak = std::weak_ptr<ActiveState>(state),
                                  callback = std::move(options.onDismissed)] {
        if (auto active = weak.lock()) active->transientHandle = 0;
        if (callback) callback();
    };

    if (!m_popupCanvasFactory) return {};
    auto canvas = m_popupCanvasFactory();
    if (!canvas) return {};

    auto popup = std::make_unique<WindowApp>(
        std::move(canvas), geometry.width, geometry.height, "LCL Popover");
    popup->setTheme(m_window.getTheme());
    const uint32_t popupSurfaceId = allocatePopupSurfaceId(m_window.getSurfaceId());
    popup->setSurfaceId(popupSurfaceId);
    popup->setAppId(m_window.getAppId().empty()
        ? "org.lcl.popover"
        : m_window.getAppId());
    popup->configurePopupSurface(
        m_window.getSurfaceId(), lcl::protocol::LCLPopupRole::Transient,
        geometry.x, geometry.y);
    const auto panelStyle = lcl::theme::resolveStyle(
        popup->getTheme().popover, lcl::theme::StyleState::Normal);
    popup->setWindowCornerStyle(panelStyle.cornerRadius, 2.0f);
    popup->setRootWidget(makePanel(
        std::move(content), {0.0f, 0.0f, geometry.width, geometry.height}));
    WindowApp* popupWindow = popup.get();
    state->popupWindow = popupWindow;
    state->popupWindowLifetime = popupWindow->getLifetimeToken();

    const bool connected = m_popupConnector
        ? m_popupConnector(*popup)
        : popup->connectCompositor(m_window.getCompositorSocketPath());
    if (!connected) return {};

    state->hostedSurfaceHandle = m_window.hostSurface(
        std::move(popup), [weak = std::weak_ptr<ActiveState>(state)] {
            auto active = weak.lock();
            if (!active) return;
            Popover::restoreFocus(active);
            const TransientHandle handle = active->transientHandle;
            active->transientHandle = 0;
            active->hostedSurfaceHandle = 0;
            if (handle != 0 && !active->windowLifetime.expired()) {
                active->window->removeTransient(handle);
            }
        });
    if (state->hostedSurfaceHandle == 0) return {};

    state->transientHandle = m_window.registerSurfaceTransient(
        popupSurfaceId,
        [weak = std::weak_ptr<ActiveState>(state)] {
            auto active = weak.lock();
            if (!active) return;
            Popover::restoreFocus(active);
            const HostedSurfaceHandle hosted = active->hostedSurfaceHandle;
            active->hostedSurfaceHandle = 0;
            active->transientHandle = 0;
            if (hosted != 0 && !active->windowLifetime.expired()) {
                active->window->removeHostedSurface(hosted);
            }
        },
        std::move(transientOptions));
    if (state->transientHandle == 0) {
        m_window.removeHostedSurface(state->hostedSurfaceHandle);
        return {};
    }

    m_active = state;
    popupWindow->getDispatcher().moveFocus(popupWindow->getRootWidget());
    if (m_active != state || state->popupWindowLifetime.expired()) return {};
    state->focusHandedOff = true;
    m_window.getDispatcher().setFocus(nullptr);
    if (m_active != state) return {};

    const PopoverOpenResult result{
        state->transientHandle, state->presentation, geometry};
    if (options.onOpened) options.onOpened(result.presentation);
    return result;
}

void Popover::restoreFocus(const std::shared_ptr<ActiveState>& state) {
    if (!state || !state->focusHandedOff || state->focusRestored) return;
    state->focusRestored = true;

    if (state->popupWindow && !state->popupWindowLifetime.expired()) {
        state->popupWindow->getDispatcher().setFocus(nullptr);
    }
    if (!state->window || state->windowLifetime.expired()) return;

    Widget* target = state->restoreTarget;
    if (!target || state->restoreTargetLifetime.expired()) target = nullptr;
    state->window->getDispatcher().setFocus(target);
}

bool Popover::close(TransientHandle handle) {
    if (handle == 0 || m_windowLifetime.expired() ||
        !m_active || m_active->transientHandle != handle) {
        return false;
    }
    const bool removed = m_window.removeTransient(handle);
    m_active.reset();
    return removed;
}

bool Popover::isOpen(TransientHandle handle) const {
    return handle != 0 && !m_windowLifetime.expired() &&
        m_active && m_active->transientHandle == handle &&
        m_window.getTransientController().contains(handle);
}

std::optional<PopoverPresentation> Popover::presentation(
        TransientHandle handle) const {
    if (!isOpen(handle)) return std::nullopt;
    return m_active->presentation;
}

void Popover::closeActive() {
    if (!m_active) return;
    const TransientHandle handle = m_active->transientHandle;
    if (handle != 0 && !m_windowLifetime.expired()) {
        m_window.removeTransient(handle);
    }
    m_active.reset();
}

} // namespace lcl::ui
