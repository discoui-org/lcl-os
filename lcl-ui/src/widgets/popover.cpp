#include "lcl-ui/widgets/popover.hpp"

#include "lcl-ui/core/canvas.hpp"
#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/container.hpp"
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

std::unique_ptr<Container> makePanel(std::unique_ptr<Widget> content,
                                     const Rect& geometry) {
    auto panel = std::make_unique<Container>();
    panel->setWidth(geometry.width);
    panel->setHeight(geometry.height);
    panel->getYogaNode().setDirection(YGFlexDirectionColumn);
    panel->setPadding(YGEdgeAll, 12.0f);
    panel->setBackgroundColor(Color{27, 32, 41, 255});
    panel->setBorderRadius(10.0f);
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
};

Popover::Popover(WindowApp& window, PopupCanvasFactory popupCanvasFactory,
                 PopupConnector popupConnector)
    : m_window(window), m_windowLifetime(window.getLifetimeToken()),
      m_popupCanvasFactory(std::move(popupCanvasFactory)),
      m_popupConnector(std::move(popupConnector)) {}

Popover::~Popover() {
    closeActive();
}

Rect Popover::placeBelowLeft(const Rect& anchorRect,
                             float width, float height) noexcept {
    return {anchorRect.x, anchorRect.y + anchorRect.height, width, height};
}

PopoverOpenResult Popover::show(Widget& anchor,
                                std::unique_ptr<Widget> content,
                                PopoverOptions options) {
    return showImpl(anchor.getAbsoluteBounds(), anchor.getLifetimeToken(), true,
                    std::move(content), std::move(options));
}

PopoverOpenResult Popover::show(const Rect& anchorRect,
                                std::unique_ptr<Widget> content,
                                PopoverOptions options) {
    return showImpl(anchorRect, {}, false, std::move(content), std::move(options));
}

PopoverOpenResult Popover::showImpl(
        const Rect& anchorRect, std::weak_ptr<uint8_t> ownerLifetime,
        bool trackOwnerLifetime, std::unique_ptr<Widget> content,
        PopoverOptions options) {
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

    const Rect geometry = placeBelowLeft(anchorRect, options.width, options.height);
    auto state = std::make_shared<ActiveState>();
    state->window = &m_window;
    state->windowLifetime = m_windowLifetime;
    state->presentation = PopoverPresentation::PopupSurface;

    TransientOptions transientOptions{};
    transientOptions.dismissOnOutsidePointer = options.dismissOnOutsidePointer;
    transientOptions.trackOwnerLifetime = trackOwnerLifetime;
    transientOptions.ownerLifetime = std::move(ownerLifetime);
    transientOptions.onDismiss = [weak = std::weak_ptr<ActiveState>(state),
                                  callback = std::move(options.onDismissed)] {
        if (auto active = weak.lock()) active->transientHandle = 0;
        if (callback) callback();
    };

    if (!m_popupCanvasFactory) return {};
    auto canvas = m_popupCanvasFactory();
    if (!canvas) return {};

    const auto popupWidth = static_cast<uint32_t>(std::ceil(geometry.width));
    const auto popupHeight = static_cast<uint32_t>(std::ceil(geometry.height));
    auto popup = std::make_unique<WindowApp>(
        std::move(canvas), popupWidth, popupHeight, "LCL Popover");
    const uint32_t popupSurfaceId = allocatePopupSurfaceId(m_window.getSurfaceId());
    popup->setSurfaceId(popupSurfaceId);
    popup->setAppId(m_window.getAppId().empty()
        ? "org.lcl.popover"
        : m_window.getAppId());
    popup->configurePopupSurface(
        m_window.getSurfaceId(), lcl::protocol::LCLPopupRole::Transient,
        static_cast<int32_t>(std::lround(geometry.x)),
        static_cast<int32_t>(std::lround(geometry.y)));
    popup->setWindowCornerStyle(10.0f, 2.0f);
    popup->setRootWidget(makePanel(
        std::move(content), {0.0f, 0.0f, geometry.width, geometry.height}));

    const bool connected = m_popupConnector
        ? m_popupConnector(*popup)
        : popup->connectCompositor(m_window.getCompositorSocketPath());
    if (!connected) return {};

    state->hostedSurfaceHandle = m_window.hostSurface(
        std::move(popup), [weak = std::weak_ptr<ActiveState>(state)] {
            auto active = weak.lock();
            if (!active) return;
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
    const PopoverOpenResult result{
        state->transientHandle, state->presentation, geometry};
    if (options.onOpened) options.onOpened(result.presentation);
    return result;
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
