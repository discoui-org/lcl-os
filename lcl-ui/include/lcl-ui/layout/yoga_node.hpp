#pragma once

#include <yoga/Yoga.h>
#include <functional>
#include <utility>

namespace lcl::ui {

using MeasureCallback = std::function<YGSize(float width, YGMeasureMode widthMode, float height, YGMeasureMode heightMode)>;

class YogaNode {
public:
    using LayoutInvalidationCallback = std::function<void()>;

    YogaNode();
    explicit YogaNode(YGNodeRef node);
    ~YogaNode();

    YogaNode(const YogaNode&) = delete;
    YogaNode& operator=(const YogaNode&) = delete;

    YogaNode(YogaNode&& other) noexcept;
    YogaNode& operator=(YogaNode&& other) noexcept;

    YGNodeRef getRef() const { return m_node; }

    // Flexbox style setters (direct Yoga C API wrappers)
    void setDirection(YGFlexDirection direction) { mutateLayout([&] { YGNodeStyleSetFlexDirection(m_node, direction); }); }
    void setJustifyContent(YGJustify justify) { mutateLayout([&] { YGNodeStyleSetJustifyContent(m_node, justify); }); }
    void setAlignItems(YGAlign align) { mutateLayout([&] { YGNodeStyleSetAlignItems(m_node, align); }); }
    void setAlignSelf(YGAlign align) { mutateLayout([&] { YGNodeStyleSetAlignSelf(m_node, align); }); }
    void setPositionType(YGPositionType positionType) { mutateLayout([&] { YGNodeStyleSetPositionType(m_node, positionType); }); }
    void setFlexWrap(YGWrap wrap) { mutateLayout([&] { YGNodeStyleSetFlexWrap(m_node, wrap); }); }

    void setFlexGrow(float flexGrow) { mutateLayout([&] { YGNodeStyleSetFlexGrow(m_node, flexGrow); }); }
    void setFlexShrink(float flexShrink) { mutateLayout([&] { YGNodeStyleSetFlexShrink(m_node, flexShrink); }); }
    void setFlexBasis(float flexBasis) { mutateLayout([&] { YGNodeStyleSetFlexBasis(m_node, flexBasis); }); }
    void setFlexBasisAuto() { mutateLayout([&] { YGNodeStyleSetFlexBasisAuto(m_node); }); }

    void setWidth(float width) { mutateLayout([&] { YGNodeStyleSetWidth(m_node, width); }); }
    void setWidthAuto() { mutateLayout([&] { YGNodeStyleSetWidthAuto(m_node); }); }
    void setHeight(float height) { mutateLayout([&] { YGNodeStyleSetHeight(m_node, height); }); }
    void setHeightAuto() { mutateLayout([&] { YGNodeStyleSetHeightAuto(m_node); }); }

    void setMinWidth(float minWidth) { mutateLayout([&] { YGNodeStyleSetMinWidth(m_node, minWidth); }); }
    void setMinHeight(float minHeight) { mutateLayout([&] { YGNodeStyleSetMinHeight(m_node, minHeight); }); }
    void setMaxWidth(float maxWidth) { mutateLayout([&] { YGNodeStyleSetMaxWidth(m_node, maxWidth); }); }
    void setMaxHeight(float maxHeight) { mutateLayout([&] { YGNodeStyleSetMaxHeight(m_node, maxHeight); }); }

    void setPadding(YGEdge edge, float padding) { mutateLayout([&] { YGNodeStyleSetPadding(m_node, edge, padding); }); }
    void setMargin(YGEdge edge, float margin) { mutateLayout([&] { YGNodeStyleSetMargin(m_node, edge, margin); }); }
    void setGap(YGGutter gutter, float gap) { mutateLayout([&] { YGNodeStyleSetGap(m_node, gutter, gap); }); }
    void setPosition(YGEdge edge, float position) { mutateLayout([&] { YGNodeStyleSetPosition(m_node, edge, position); }); }

    // Hierarchy management
    void insertChild(YogaNode* child, uint32_t index);
    void appendChild(YogaNode* child);
    void removeChild(YogaNode* child);
    void removeAllChildren();

    // Layout calculation
    void calculateLayout(float parentWidth = YGUndefined, float parentHeight = YGUndefined, YGDirection direction = YGDirectionLTR);

    // Calculated layout getters
    float getLayoutX() const { return YGNodeLayoutGetLeft(m_node); }
    float getLayoutY() const { return YGNodeLayoutGetTop(m_node); }
    float getLayoutWidth() const { return YGNodeLayoutGetWidth(m_node); }
    float getLayoutHeight() const { return YGNodeLayoutGetHeight(m_node); }

    // Measurement callback wrapper
    void setMeasureFunc(MeasureCallback callback);
    void markDirty();
    void setLayoutInvalidationCallback(LayoutInvalidationCallback callback) {
        m_layoutInvalidationCallback = std::move(callback);
    }

private:
    template <typename Mutation>
    void mutateLayout(Mutation&& mutation) {
        if (!m_node) return;
        const bool wasDirty = YGNodeIsDirty(m_node);
        std::forward<Mutation>(mutation)();
        if (!wasDirty && YGNodeIsDirty(m_node)) notifyLayoutMutation();
    }

    void notifyLayoutMutation() {
        if (m_layoutInvalidationCallback) m_layoutInvalidationCallback();
    }

    YGNodeRef m_node{nullptr};
    MeasureCallback m_measureCallback{nullptr};
    LayoutInvalidationCallback m_layoutInvalidationCallback{nullptr};
    static YGSize staticMeasureFunc(YGNodeRef node, float width, YGMeasureMode widthMode, float height, YGMeasureMode heightMode);
};

} // namespace lcl::ui
