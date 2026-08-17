#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace lcl::ui {

class EventDispatcher;
class Widget;
struct PointerEvent;

using TransientHandle = uint64_t;

struct TransientOptions {
    bool dismissOnOutsidePointer{false};
    std::function<void()> onDismiss;
};

/**
 * Owns transient lifetime and dismissal policy without becoming a Widget or
 * introducing another layout/render tree.
 *
 * Local entries are ordinary absolute-positioned children of WindowApp's
 * normal WindowRoot. Surface entries hold only a surface identity and teardown
 * callback; their pixels, geometry, and input remain compositor concerns.
 */
class TransientController final {
public:
    explicit TransientController(EventDispatcher& dispatcher);
    ~TransientController();

    TransientController(const TransientController&) = delete;
    TransientController& operator=(const TransientController&) = delete;

    void setWindowRoot(Widget* windowRoot);

    TransientHandle registerLocal(std::unique_ptr<Widget> widget,
                                  TransientOptions options = {});
    TransientHandle registerSurface(uint32_t surfaceId,
                                    std::function<void()> destroySurface,
                                    TransientOptions options = {});
    bool remove(TransientHandle handle);
    void clear();

    size_t size() const noexcept { return m_entries.size(); }
    bool containsLocalTarget(const Widget* target) const;

    /** Called after mouse-down or a validated touch tap has been established. */
    bool dismissTopmostOnOutsidePointer(Widget* hitTarget,
                                        const PointerEvent& event);

private:
    enum class Presentation { LocalWidget, PopupSurface };

    struct Entry {
        TransientHandle handle{0};
        Presentation presentation{Presentation::LocalWidget};
        Widget* widget{nullptr};
        uint32_t surfaceId{0};
        std::function<void()> destroySurface;
        TransientOptions options;
    };

    static bool containsWidget(const Widget* ancestor, const Widget* target);
    bool removeInternal(TransientHandle handle, bool notifyDismiss);

    EventDispatcher& m_dispatcher;
    Widget* m_windowRoot{nullptr};
    std::vector<Entry> m_entries;
    TransientHandle m_nextHandle{1};
};

} // namespace lcl::ui
