#pragma once

#include "lcl-ui/core/rect.hpp"
#include "lcl-ui/core/effects.hpp"
#include "lcl-ui/core/render_pass.hpp"
#include "lcl-ui/core/events.hpp"
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

    void setFocusable(bool focusable) { m_focusable = focusable; }
    bool isFocusable() const { return m_focusable; }

    void markDirty();
    void setRenderPass(RenderPass* pass) { m_renderPass = pass; }

    using GcMarkCallback = std::function<void(void* rt, void* mark_func)>;
    void setGcMarkCallback(GcMarkCallback cb) { m_gcMarkCallback = std::move(cb); }
    const GcMarkCallback& getGcMarkCallback() const { return m_gcMarkCallback; }

    virtual void syncLayout(float parentAbsX = 0.0f, float parentAbsY = 0.0f);
    virtual void draw(SkCanvas* canvas, const Rect& damageRect);
    virtual void collectEffects(std::vector<EffectRegion>& outEffects) const;

    // Polymorphic Event Handlers (Return true if handled, false to bubble to parent)
    virtual bool onPointerEnter(const PointerEvent& event) { (void)event; return false; }
    virtual bool onPointerLeave(const PointerEvent& event) { (void)event; return false; }
    virtual bool onPointerDown(const PointerEvent& event) { (void)event; return false; }
    virtual bool onPointerUp(const PointerEvent& event) { (void)event; return false; }
    virtual bool onPointerMove(const PointerEvent& event) { (void)event; return false; }
    virtual bool onScroll(const PointerEvent& event) { (void)event; return false; }
    virtual bool onKeyDown(const KeyEvent& event) { (void)event; return false; }
    virtual bool onKeyUp(const KeyEvent& event) { (void)event; return false; }
    virtual bool onTextInput(const TextInputEvent& event) { (void)event; return false; }
    virtual bool onFocusGained(const FocusEvent& event) { (void)event; return false; }
    virtual bool onFocusLost(const FocusEvent& event) { (void)event; return false; }

protected:
    YogaNode m_yogaNode;
    Widget* m_parent{nullptr};
    std::vector<std::unique_ptr<Widget>> m_children;

    Rect m_bounds{0.0f, 0.0f, 0.0f, 0.0f};
    Rect m_absoluteBounds{0.0f, 0.0f, 0.0f, 0.0f};
    bool m_visible{true};
    bool m_focusable{false};
    RenderPass* m_renderPass{nullptr};
    GcMarkCallback m_gcMarkCallback{nullptr};
};

} // namespace lcl::ui
