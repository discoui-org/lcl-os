#pragma once

#include "lcl-ui/widgets/popover.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lcl::ui {

struct MenuItem {
    std::string label;
    bool enabled{true};
    std::function<void()> onActivate;
};

struct MenuOptions {
    float width{220.0f};
    float itemHeight{40.0f};
    std::function<void()> onDismissed;
};

/** A focused list of actions presented through the existing Popover path. */
class Menu final {
public:
    using PopupCanvasFactory = Popover::PopupCanvasFactory;
    using PopupConnector = Popover::PopupConnector;

    explicit Menu(WindowApp& window,
                  PopupCanvasFactory popupCanvasFactory,
                  PopupConnector popupConnector = {});
    ~Menu();

    Menu(const Menu&) = delete;
    Menu& operator=(const Menu&) = delete;

    PopoverOpenResult show(Widget& anchor, std::vector<MenuItem> items,
                           MenuOptions options = {});
    bool close(TransientHandle handle);
    bool isOpen(TransientHandle handle) const;

private:
    struct Session;

    static void activateItem(const std::weak_ptr<Session>& weakSession,
                             size_t index);
    static void closeSession(const std::weak_ptr<Session>& weakSession);
    void closeActive();

    std::shared_ptr<uint8_t> m_lifetimeToken{std::make_shared<uint8_t>(0)};
    Popover m_popover;
    std::shared_ptr<Session> m_active;
};

} // namespace lcl::ui
