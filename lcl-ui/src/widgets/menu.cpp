#include "lcl-ui/widgets/menu.hpp"

#include "lcl-ui/widgets/button.hpp"
#include "lcl-ui/widgets/container.hpp"

#include <cmath>
#include <optional>
#include <utility>

namespace lcl::ui {
namespace {

class MenuContent;

class MenuItemButton final : public Button {
public:
    MenuItemButton(std::string label, MenuContent& menu, size_t index,
                   bool selected)
        : Button(label), m_menu(menu), m_index(index), m_selected(selected) {}

    bool onPointerUp(const PointerEvent& event) override;
    bool onFocusGained(const FocusEvent& event) override;
    void draw(graphics::Canvas& canvas,
              const graphics::RectF& damageRect) override;
    bool shouldFocusOnTouchTap(const PointerEvent& event) const override {
        (void)event;
        // Activation closes the popup during PointerUp; do not re-focus a
        // closing item in EventDispatcher's post-dispatch touch-focus phase.
        return false;
    }

private:
    const lcl::theme::WidgetStyle* defaultStyle() const noexcept override {
        return &getTheme().menuItem;
    }

    MenuContent& m_menu;
    size_t m_index{0};
    bool m_selected{false};
};

class MenuContent final : public Container {
public:
    MenuContent(const std::vector<MenuItem>& items, float width,
                float itemHeight, std::function<void(size_t)> activate,
                std::function<void()> close)
        : m_activate(std::move(activate)), m_close(std::move(close)) {
        setDirection(layout::Direction::Column);
        setGap(layout::Gutter::All, 2.0f);
        setWidth(width);

        m_items.reserve(items.size());
        for (size_t index = 0; index < items.size(); ++index) {
            auto button = std::make_unique<MenuItemButton>(
                items[index].label, *this, index, items[index].selected);
            MenuItemButton* item = button.get();
            item->setWidth(width);
            item->setHeight(itemHeight);
            item->setJustifyContent(layout::Justify::FlexStart);
            item->setEnabled(items[index].enabled);
            item->setOnClick([activate = m_activate, index] {
                if (activate) activate(index);
            });
            m_items.push_back(item);
            addChild(std::move(button));
        }
    }

    void noteFocused(size_t index) {
        if (index < m_items.size() && m_items[index]->isEnabled()) {
            m_focusedIndex = index;
        }
    }

    bool onKeyDown(const KeyEvent& event) override {
        switch (event.key) {
            case lcl::platform::PhysicalKey::ArrowDown:
                return moveFocus(event, false);
            case lcl::platform::PhysicalKey::ArrowUp:
                return moveFocus(event, true);
            case lcl::platform::PhysicalKey::Enter:
            case lcl::platform::PhysicalKey::Space:
                if (m_focusedIndex && *m_focusedIndex < m_items.size() &&
                    m_items[*m_focusedIndex]->isEnabled() && m_activate) {
                    m_activate(*m_focusedIndex);
                }
                return true;
            case lcl::platform::PhysicalKey::Escape:
                if (m_close) m_close();
                return true;
            default:
                return false;
        }
    }

private:
    bool moveFocus(const KeyEvent& event, bool backwards) {
        if (m_items.empty()) return true;

        const size_t count = m_items.size();
        const size_t start = m_focusedIndex.value_or(
            backwards ? 0 : count - 1);
        for (size_t step = 1; step <= count; ++step) {
            const size_t index = backwards
                ? (start + count - (step % count)) % count
                : (start + step) % count;
            if (!m_items[index]->isEnabled()) continue;
            if (event.requestFocus(*m_items[index])) {
                m_focusedIndex = index;
            }
            return true;
        }
        return true;
    }

    std::vector<MenuItemButton*> m_items;
    std::optional<size_t> m_focusedIndex;
    std::function<void(size_t)> m_activate;
    std::function<void()> m_close;
};

bool MenuItemButton::onPointerUp(const PointerEvent& event) {
    if (event.source == PointerSource::Touch &&
        !event.isTouchTapCompletion()) {
        Button::onPointerCancel(event);
        return true;
    }
    return Button::onPointerUp(event);
}

bool MenuItemButton::onFocusGained(const FocusEvent& event) {
    const bool handled = Button::onFocusGained(event);
    m_menu.noteFocused(m_index);
    return handled;
}

void MenuItemButton::draw(graphics::Canvas& canvas,
                          const graphics::RectF& damageRect) {
    Button::draw(canvas, damageRect);
    if (!m_selected || !m_visible ||
        !getPresentationPaintBounds().intersects(damageRect)) return;

    beginPresentation(canvas, damageRect);
    const float right = m_absoluteBounds.x + m_absoluteBounds.width - 10.0f;
    const float centerY = m_absoluteBounds.y + m_absoluteBounds.height * 0.5f;
    graphics::Path mark;
    mark.moveTo(right - 8.0f, centerY)
        .lineTo(right - 5.0f, centerY + 3.0f)
        .lineTo(right, centerY - 3.5f);
    graphics::Paint paint;
    paint.color = getTheme().colors.accent;
    paint.style = graphics::PaintStyle::Stroke;
    paint.stroke.width = 1.75f;
    paint.stroke.cap = graphics::StrokeCap::Round;
    paint.stroke.join = graphics::StrokeJoin::Round;
    canvas.drawPath(mark, paint);
    endPresentation(canvas);
}

} // namespace

struct Menu::Session {
    Menu* menu{nullptr};
    std::weak_ptr<uint8_t> menuLifetime;
    std::vector<MenuItem> items;
    TransientHandle handle{0};
};

Menu::Menu(WindowApp& window, PopupCanvasFactory popupCanvasFactory,
           PopupConnector popupConnector)
    : m_popover(window, std::move(popupCanvasFactory),
                std::move(popupConnector)) {}

Menu::~Menu() {
    m_lifetimeToken.reset();
    closeActive();
}

PopoverOpenResult Menu::show(Widget& anchor, std::vector<MenuItem> items,
                             MenuOptions options) {
    closeActive();
    if (!std::isfinite(options.width) || !std::isfinite(options.itemHeight) ||
        options.width <= 24.0f || options.itemHeight <= 0.0f) {
        return {};
    }

    auto session = std::make_shared<Session>();
    session->menu = this;
    session->menuLifetime = m_lifetimeToken;
    session->items = std::move(items);
    const std::weak_ptr<Session> weakSession = session;
    const float contentWidth = options.width - 24.0f;
    auto content = std::make_unique<MenuContent>(
        session->items, contentWidth, options.itemHeight,
        [weakSession](size_t index) { Menu::activateItem(weakSession, index); },
        [weakSession] { Menu::closeSession(weakSession); });

    const size_t itemCount = session->items.size();
    const float contentHeight = itemCount == 0
        ? 0.0f
        : static_cast<float>(itemCount) * options.itemHeight +
            static_cast<float>(itemCount - 1) * 2.0f;
    auto dismissed = std::move(options.onDismissed);
    const auto result = m_popover.show(
        anchor, std::move(content),
        PopoverOptions{
            .width = options.width,
            .height = 24.0f + contentHeight,
            .onOpened = {},
            .onDismissed = [weakSession, dismissed = std::move(dismissed)] {
                if (auto active = weakSession.lock(); active &&
                    !active->menuLifetime.expired() &&
                    active->menu->m_active == active) {
                    active->menu->m_active.reset();
                }
                if (dismissed) dismissed();
            },
        });
    if (!result) return {};

    session->handle = result.handle;
    m_active = session;
    return result;
}

bool Menu::close(TransientHandle handle) {
    if (handle == 0 || !m_active || m_active->handle != handle) return false;
    const auto active = m_active;
    const bool closed = m_popover.close(handle);
    if (m_active == active) m_active.reset();
    return closed;
}

bool Menu::isOpen(TransientHandle handle) const {
    return handle != 0 && m_active && m_active->handle == handle &&
        m_popover.isOpen(handle);
}

void Menu::activateItem(const std::weak_ptr<Session>& weakSession,
                        size_t index) {
    const auto session = weakSession.lock();
    if (!session || index >= session->items.size() ||
        !session->items[index].enabled) {
        return;
    }

    const auto callback = session->items[index].onActivate;
    const auto menuLifetime = session->menuLifetime;
    Menu* menu = session->menu;
    const TransientHandle handle = session->handle;
    if (callback) callback();
    if (!menuLifetime.expired()) menu->close(handle);
}

void Menu::closeSession(const std::weak_ptr<Session>& weakSession) {
    const auto session = weakSession.lock();
    if (!session || session->menuLifetime.expired()) return;
    session->menu->close(session->handle);
}

void Menu::closeActive() {
    if (!m_active) return;
    const TransientHandle handle = m_active->handle;
    if (handle != 0) m_popover.close(handle);
    m_active.reset();
}

} // namespace lcl::ui
