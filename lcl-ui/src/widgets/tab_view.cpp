#include "lcl-ui/widgets/tab_view.hpp"

#include "lcl-graphics/canvas.hpp"

#include <algorithm>
#include <utility>

namespace lcl::ui {
namespace {

constexpr float kTextSize = 12.0f;
constexpr float kItemHorizontalInset = 5.0f;

} // namespace

TabView::TabView() {
    setClipsToBounds(true);
    setFocusable(true);
}

void TabView::addTab(Tab tab) {
    if (!tab.content) return;
    Widget* content = tab.content.get();
    content->getYogaNode().setPositionType(YGPositionTypeAbsolute);
    content->setPosition(YGEdgeLeft, 0.0f);
    content->setPosition(YGEdgeTop, 0.0f);
    content->setPosition(YGEdgeRight, 0.0f);
    content->setPosition(YGEdgeBottom, getTheme().metrics.tabBarHeight);
    m_tabs.push_back(TabState{std::move(tab.title), content, tab.enabled});
    addChild(std::move(tab.content));
    updateContentVisibility();
    markDirty();
}

void TabView::setSelectedIndex(size_t index) {
    if (index >= m_tabs.size() || !m_tabs[index].enabled ||
        index == m_selectedIndex) return;
    m_selectedIndex = index;
    updateContentVisibility();
    markDirty();
    auto callback = m_onChange;
    if (callback) callback(index);
}

void TabView::setTabViewStyle(TabViewStyle style) {
    if (m_style == style) return;
    m_style = style;
    markDirty();
}

void TabView::updateContentVisibility() {
    if (!m_tabs.empty() && m_selectedIndex >= m_tabs.size()) {
        m_selectedIndex = 0;
    }
    for (size_t index = 0; index < m_tabs.size(); ++index) {
        m_tabs[index].content->setVisible(index == m_selectedIndex);
    }
}

void TabView::updateContentInsets() {
    for (auto& tab : m_tabs) {
        tab.content->setPosition(YGEdgeBottom, getTheme().metrics.tabBarHeight);
    }
}

std::optional<size_t> TabView::tabIndexAt(float x, float y) const noexcept {
    if (m_tabs.empty() || x < m_absoluteBounds.x ||
        x >= m_absoluteBounds.x + m_absoluteBounds.width ||
        y < m_absoluteBounds.y + m_absoluteBounds.height -
                getTheme().metrics.tabBarHeight ||
        y >= m_absoluteBounds.y + m_absoluteBounds.height) {
        return std::nullopt;
    }
    const float itemWidth = m_absoluteBounds.width /
        static_cast<float>(m_tabs.size());
    const size_t index = static_cast<size_t>((x - m_absoluteBounds.x) /
                                             std::max(1.0f, itemWidth));
    return index < m_tabs.size() ? std::optional<size_t>(index)
                                 : std::nullopt;
}

bool TabView::onPointerEnter(const PointerEvent& event) {
    if (event.source == PointerSource::Mouse) {
        m_hoveredIndex = tabIndexAt(event.x, event.y);
        markDirty();
    }
    return m_hoveredIndex.has_value();
}

bool TabView::onPointerLeave(const PointerEvent&) {
    m_hoveredIndex.reset();
    m_pressedIndex.reset();
    markDirty();
    return true;
}

bool TabView::onPointerDown(const PointerEvent& event) {
    if (event.source == PointerSource::Mouse && event.button != 0) return false;
    const auto index = tabIndexAt(event.x, event.y);
    if (!index || !m_tabs[*index].enabled) return false;
    m_pressedIndex = index;
    markDirty();
    return true;
}

bool TabView::onPointerMove(const PointerEvent& event) {
    const auto index = tabIndexAt(event.x, event.y);
    if (index != m_hoveredIndex) {
        m_hoveredIndex = index;
        markDirty();
    }
    return m_pressedIndex.has_value();
}

bool TabView::onPointerUp(const PointerEvent& event) {
    const auto index = tabIndexAt(event.x, event.y);
    const bool activate = m_pressedIndex && index &&
        *m_pressedIndex == *index &&
        (event.source == PointerSource::Mouse
            ? event.button == 0
            : event.isTouchTapCompletion());
    const bool handled = m_pressedIndex.has_value();
    m_pressedIndex.reset();
    m_hoveredIndex = event.source == PointerSource::Mouse ? index
                                                          : std::nullopt;
    markDirty();
    if (activate) setSelectedIndex(*index);
    return handled;
}

bool TabView::onPointerCancel(const PointerEvent&) {
    m_hoveredIndex.reset();
    m_pressedIndex.reset();
    markDirty();
    return true;
}

bool TabView::moveSelection(int direction) {
    if (m_tabs.empty()) return true;
    const size_t count = m_tabs.size();
    for (size_t step = 1; step <= count; ++step) {
        const size_t index = direction < 0
            ? (m_selectedIndex + count - (step % count)) % count
            : (m_selectedIndex + step) % count;
        if (!m_tabs[index].enabled) continue;
        setSelectedIndex(index);
        return true;
    }
    return true;
}

bool TabView::onKeyDown(const KeyEvent& event) {
    switch (event.key) {
        case lcl::platform::PhysicalKey::ArrowLeft:
        case lcl::platform::PhysicalKey::ArrowUp:
            return moveSelection(-1);
        case lcl::platform::PhysicalKey::ArrowRight:
        case lcl::platform::PhysicalKey::ArrowDown:
            return moveSelection(1);
        case lcl::platform::PhysicalKey::Home:
            for (size_t index = 0; index < m_tabs.size(); ++index) {
                if (m_tabs[index].enabled) {
                    setSelectedIndex(index);
                    break;
                }
            }
            return true;
        case lcl::platform::PhysicalKey::End:
            for (size_t index = m_tabs.size(); index > 0; --index) {
                if (m_tabs[index - 1].enabled) {
                    setSelectedIndex(index - 1);
                    break;
                }
            }
            return true;
        default:
            return false;
    }
}

bool TabView::onFocusGained(const FocusEvent&) {
    m_focused = true;
    markDirty();
    return false;
}

bool TabView::onFocusLost(const FocusEvent&) {
    m_focused = false;
    m_pressedIndex.reset();
    markDirty();
    return false;
}

const lcl::theme::WidgetStyle* TabView::defaultStyle() const noexcept {
    return &getTheme().tabView;
}

void TabView::styleDidChange() {
    updateContentInsets();
    markDirty();
}

void TabView::draw(graphics::Canvas& canvas,
                   const graphics::RectF& damageRect) {
    if (!m_visible || !getPresentationPaintBounds().intersects(damageRect)) return;
    beginPresentation(canvas);
    drawChildren(canvas, damageRect);

    const auto visual = lcl::theme::resolveStyle(
        *resolvedStyle(), lcl::theme::StyleState::Normal);
    const float barHeight = std::min(getTheme().metrics.tabBarHeight,
                                     m_absoluteBounds.height);
    const graphics::RectF bar{
        m_absoluteBounds.x,
        m_absoluteBounds.y + m_absoluteBounds.height - barHeight,
        m_absoluteBounds.width, barHeight};
    canvas.drawRect(bar, visual.background);
    canvas.drawRect({bar.x, bar.y, bar.width,
                     std::max(1.0f, visual.borderWidth)}, visual.border);

    if (!m_tabs.empty()) {
        const float itemWidth = bar.width / static_cast<float>(m_tabs.size());
        for (size_t index = 0; index < m_tabs.size(); ++index) {
            const graphics::RectF item{
                bar.x + itemWidth * static_cast<float>(index) +
                    kItemHorizontalInset,
                bar.y + 5.0f,
                std::max(0.0f, itemWidth - kItemHorizontalInset * 2.0f),
                std::max(0.0f, bar.height - 10.0f)};
            if (m_hoveredIndex == index || m_pressedIndex == index) {
                canvas.drawRoundedRect(
                    item, getTheme().metrics.compactCornerRadius,
                    m_pressedIndex == index
                        ? getTheme().colors.selectionFillPressed
                        : getTheme().colors.selectionFill,
                    {}, 0.0f, 1.0f);
            }
            const graphics::Color color = !m_tabs[index].enabled
                ? getTheme().colors.tertiaryLabel
                : index == m_selectedIndex ? visual.accent
                                           : visual.foreground;
            const float textWidth = canvas.measureText(m_tabs[index].title,
                                                       kTextSize);
            canvas.drawText(item.x +
                                std::max(0.0f, (item.width - textWidth) * 0.5f),
                            item.y +
                                std::max(0.0f, (item.height - kTextSize * 1.2f) * 0.5f),
                            m_tabs[index].title, color, kTextSize);
        }
    }
    if (m_focused) {
        canvas.drawRoundedRect(bar,
                               getTheme().metrics.compactCornerRadius,
                               {}, getTheme().colors.focusRing, 1.5f, 1.0f);
    }
    endPresentation(canvas);
}

} // namespace lcl::ui
