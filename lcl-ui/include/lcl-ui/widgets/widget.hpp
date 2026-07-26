#pragma once

#include "lcl-ui/core/rect.hpp"
#include "lcl-ui/core/render_pass.hpp"
#include "lcl-ui/layout/yoga_node.hpp"
#include <vector>
#include <memory>
#include <string>

class SkCanvas;

namespace lcl::ui {

class Widget {
public:
    Widget();
    virtual ~Widget() = default;

    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    YogaNode& getYogaNode() { return m_yogaNode; }
    const YogaNode& getYogaNode() const { return m_yogaNode; }

    void addChild(std::unique_ptr<Widget> child);
    void removeChild(Widget* child);
    const std::vector<std::unique_ptr<Widget>>& getChildren() const { return m_children; }

    Widget* getParent() const { return m_parent; }

    Rect getBounds() const { return m_bounds; }
    Rect getAbsoluteBounds() const { return m_absoluteBounds; }

    void setVisible(bool visible) { m_visible = visible; markDirty(); }
    bool isVisible() const { return m_visible; }

    void markDirty();
    void setRenderPass(RenderPass* pass) { m_renderPass = pass; }

    virtual void syncLayout(float parentAbsX = 0.0f, float parentAbsY = 0.0f);
    virtual void draw(SkCanvas* canvas, const Rect& damageRect);

    // Event handling hooks
    virtual bool onPointerMove(float px, float py);
    virtual bool onPointerDown(float px, float py);
    virtual bool onPointerUp(float px, float py);

protected:
    YogaNode m_yogaNode;
    Widget* m_parent{nullptr};
    std::vector<std::unique_ptr<Widget>> m_children;

    Rect m_bounds{0.0f, 0.0f, 0.0f, 0.0f};
    Rect m_absoluteBounds{0.0f, 0.0f, 0.0f, 0.0f};
    bool m_visible{true};
    RenderPass* m_renderPass{nullptr};
};

} // namespace lcl::ui
