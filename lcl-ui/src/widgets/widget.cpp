#include "lcl-ui/widgets/widget.hpp"
#include <algorithm>

namespace lcl::ui {

Widget::Widget() = default;

void Widget::addChild(std::unique_ptr<Widget> child) {
    if (!child) return;
    child->m_parent = this;
    child->m_renderPass = m_renderPass;
    m_yogaNode.appendChild(&child->getYogaNode());
    m_children.push_back(std::move(child));
    markDirty();
}

void Widget::removeChild(Widget* child) {
    if (!child) return;
    auto it = std::find_if(m_children.begin(), m_children.end(),
        [child](const std::unique_ptr<Widget>& ptr) { return ptr.get() == child; });
    if (it != m_children.end()) {
        m_yogaNode.removeChild(&child->getYogaNode());
        (*it)->m_parent = nullptr;
        m_children.erase(it);
        markDirty();
    }
}

void Widget::markDirty() {
    if (m_renderPass && m_visible) {
        m_renderPass->addDirtyRect(m_absoluteBounds);
    }
    if (m_parent) {
        m_parent->markDirty();
    }
}

void Widget::syncLayout(float parentAbsX, float parentAbsY) {
    m_bounds = Rect{
        m_yogaNode.getLayoutX(),
        m_yogaNode.getLayoutY(),
        m_yogaNode.getLayoutWidth(),
        m_yogaNode.getLayoutHeight()
    };

    m_absoluteBounds = Rect{
        parentAbsX + m_bounds.x,
        parentAbsY + m_bounds.y,
        m_bounds.width,
        m_bounds.height
    };

    for (auto& child : m_children) {
        child->syncLayout(m_absoluteBounds.x, m_absoluteBounds.y);
    }
}

void Widget::draw(SkCanvas* canvas, const Rect& damageRect) {
    if (!m_visible) return;

    for (auto& child : m_children) {
        if (child->isVisible() && child->getAbsoluteBounds().intersects(damageRect)) {
            child->draw(canvas, damageRect);
        }
    }
}

bool Widget::onPointerMove(float px, float py) {
    if (!m_visible || !m_absoluteBounds.containsPoint(px, py)) return false;

    for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) {
        if ((*it)->onPointerMove(px, py)) return true;
    }
    return true;
}

bool Widget::onPointerDown(float px, float py) {
    if (!m_visible || !m_absoluteBounds.containsPoint(px, py)) return false;

    for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) {
        if ((*it)->onPointerDown(px, py)) return true;
    }
    return true;
}

bool Widget::onPointerUp(float px, float py) {
    if (!m_visible || !m_absoluteBounds.containsPoint(px, py)) return false;

    for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) {
        if ((*it)->onPointerUp(px, py)) return true;
    }
    return true;
}

} // namespace lcl::ui
