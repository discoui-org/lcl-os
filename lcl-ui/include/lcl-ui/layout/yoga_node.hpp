#pragma once

#include <yoga/Yoga.h>
#include <functional>

namespace lcl::ui {

using MeasureCallback = std::function<YGSize(float width, YGMeasureMode widthMode, float height, YGMeasureMode heightMode)>;

class YogaNode {
public:
    YogaNode();
    explicit YogaNode(YGNodeRef node);
    ~YogaNode();

    YogaNode(const YogaNode&) = delete;
    YogaNode& operator=(const YogaNode&) = delete;

    YogaNode(YogaNode&& other) noexcept;
    YogaNode& operator=(YogaNode&& other) noexcept;

    YGNodeRef getRef() const { return m_node; }

    // Flexbox style setters (direct Yoga C API wrappers)
    void setDirection(YGFlexDirection direction) { YGNodeStyleSetFlexDirection(m_node, direction); }
    void setJustifyContent(YGJustify justify) { YGNodeStyleSetJustifyContent(m_node, justify); }
    void setAlignItems(YGAlign align) { YGNodeStyleSetAlignItems(m_node, align); }
    void setAlignSelf(YGAlign align) { YGNodeStyleSetAlignSelf(m_node, align); }
    void setPositionType(YGPositionType positionType) { YGNodeStyleSetPositionType(m_node, positionType); }
    void setFlexWrap(YGWrap wrap) { YGNodeStyleSetFlexWrap(m_node, wrap); }

    void setFlexGrow(float flexGrow) { YGNodeStyleSetFlexGrow(m_node, flexGrow); }
    void setFlexShrink(float flexShrink) { YGNodeStyleSetFlexShrink(m_node, flexShrink); }
    void setFlexBasis(float flexBasis) { YGNodeStyleSetFlexBasis(m_node, flexBasis); }
    void setFlexBasisAuto() { YGNodeStyleSetFlexBasisAuto(m_node); }

    void setWidth(float width) { YGNodeStyleSetWidth(m_node, width); }
    void setWidthAuto() { YGNodeStyleSetWidthAuto(m_node); }
    void setHeight(float height) { YGNodeStyleSetHeight(m_node, height); }
    void setHeightAuto() { YGNodeStyleSetHeightAuto(m_node); }

    void setMinWidth(float minWidth) { YGNodeStyleSetMinWidth(m_node, minWidth); }
    void setMinHeight(float minHeight) { YGNodeStyleSetMinHeight(m_node, minHeight); }
    void setMaxWidth(float maxWidth) { YGNodeStyleSetMaxWidth(m_node, maxWidth); }
    void setMaxHeight(float maxHeight) { YGNodeStyleSetMaxHeight(m_node, maxHeight); }

    void setPadding(YGEdge edge, float padding) { YGNodeStyleSetPadding(m_node, edge, padding); }
    void setMargin(YGEdge edge, float margin) { YGNodeStyleSetMargin(m_node, edge, margin); }
    void setGap(YGGutter gutter, float gap) { YGNodeStyleSetGap(m_node, gutter, gap); }
    void setPosition(YGEdge edge, float position) { YGNodeStyleSetPosition(m_node, edge, position); }

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

private:
    YGNodeRef m_node{nullptr};
    MeasureCallback m_measureCallback{nullptr};
    static YGSize staticMeasureFunc(YGNodeRef node, float width, YGMeasureMode widthMode, float height, YGMeasureMode heightMode);
};

} // namespace lcl::ui
