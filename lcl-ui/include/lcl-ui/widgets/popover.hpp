#pragma once

#include "lcl-graphics/canvas.hpp"
#include "lcl-ui/core/transient_controller.hpp"

#include <functional>
#include <memory>
#include <optional>

namespace lcl::ui {

class Widget;
class WindowApp;

enum class PopoverPresentation {
    PopupSurface,
};

struct PopoverOptions {
    float width{240.0f};
    float height{120.0f};
    bool dismissOnOutsidePointer{true};
    std::function<void(PopoverPresentation)> onOpened;
    std::function<void()> onDismissed;
};

struct PopoverOpenResult {
    TransientHandle handle{0};
    PopoverPresentation presentation{PopoverPresentation::PopupSurface};
    graphics::RectF geometry{};

    explicit operator bool() const noexcept { return handle != 0; }
};

/**
 * Places generic Widget content below an anchor in a parent-bound transient
 * PopupSurface. Lifecycle policy remains in TransientController while the
 * hosted surface reuses the normal WindowApp widget/render/input path.
 */
class Popover final {
public:
    using PopupCanvasFactory = std::function<std::unique_ptr<graphics::Canvas>()>;
    using PopupConnector = std::function<bool(WindowApp&)>;

    explicit Popover(WindowApp& window,
                     PopupCanvasFactory popupCanvasFactory,
                     PopupConnector popupConnector = {});
    ~Popover();

    Popover(const Popover&) = delete;
    Popover& operator=(const Popover&) = delete;

    PopoverOpenResult show(Widget& anchor, std::unique_ptr<Widget> content,
                           PopoverOptions options = {});
    PopoverOpenResult show(const graphics::RectF& anchorRect,
                           std::unique_ptr<Widget> content,
                           PopoverOptions options = {});

    bool close(TransientHandle handle);
    bool isOpen(TransientHandle handle) const;
    std::optional<PopoverPresentation> presentation(TransientHandle handle) const;

    static graphics::RectF placeBelowLeft(const graphics::RectF& anchorRect,
                               float width, float height) noexcept;

private:
    struct ActiveState;

    PopoverOpenResult showImpl(const graphics::RectF& anchorRect,
                               Widget* restoreTarget,
                               std::weak_ptr<uint8_t> ownerLifetime,
                               bool trackOwnerLifetime,
                               std::unique_ptr<Widget> content,
                               PopoverOptions options);
    static void restoreFocus(const std::shared_ptr<ActiveState>& state);
    void closeActive();

    WindowApp& m_window;
    std::weak_ptr<uint8_t> m_windowLifetime;
    PopupCanvasFactory m_popupCanvasFactory;
    PopupConnector m_popupConnector;
    std::shared_ptr<ActiveState> m_active;
};

} // namespace lcl::ui
