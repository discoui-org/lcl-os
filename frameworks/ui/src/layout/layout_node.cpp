#include "layout/layout_node.hpp"

#include <yoga/Yoga.h>

#include <utility>

namespace lcl::ui::detail {
namespace {

YGFlexDirection toYoga(layout::Direction value) {
    switch (value) {
        case layout::Direction::Row: return YGFlexDirectionRow;
        case layout::Direction::RowReverse: return YGFlexDirectionRowReverse;
        case layout::Direction::Column: return YGFlexDirectionColumn;
        case layout::Direction::ColumnReverse: return YGFlexDirectionColumnReverse;
    }
    return YGFlexDirectionColumn;
}

YGJustify toYoga(layout::Justify value) {
    switch (value) {
        case layout::Justify::FlexStart: return YGJustifyFlexStart;
        case layout::Justify::Center: return YGJustifyCenter;
        case layout::Justify::FlexEnd: return YGJustifyFlexEnd;
        case layout::Justify::SpaceBetween: return YGJustifySpaceBetween;
        case layout::Justify::SpaceAround: return YGJustifySpaceAround;
        case layout::Justify::SpaceEvenly: return YGJustifySpaceEvenly;
    }
    return YGJustifyFlexStart;
}

YGAlign toYoga(layout::Align value) {
    switch (value) {
        case layout::Align::Auto: return YGAlignAuto;
        case layout::Align::FlexStart: return YGAlignFlexStart;
        case layout::Align::Center: return YGAlignCenter;
        case layout::Align::FlexEnd: return YGAlignFlexEnd;
        case layout::Align::Stretch: return YGAlignStretch;
        case layout::Align::Baseline: return YGAlignBaseline;
        case layout::Align::SpaceBetween: return YGAlignSpaceBetween;
        case layout::Align::SpaceAround: return YGAlignSpaceAround;
    }
    return YGAlignAuto;
}

YGPositionType toYoga(layout::PositionType value) {
    switch (value) {
        case layout::PositionType::Static: return YGPositionTypeStatic;
        case layout::PositionType::Relative: return YGPositionTypeRelative;
        case layout::PositionType::Absolute: return YGPositionTypeAbsolute;
    }
    return YGPositionTypeRelative;
}

layout::PositionType fromYoga(YGPositionType value) {
    switch (value) {
        case YGPositionTypeStatic: return layout::PositionType::Static;
        case YGPositionTypeRelative: return layout::PositionType::Relative;
        case YGPositionTypeAbsolute: return layout::PositionType::Absolute;
    }
    return layout::PositionType::Relative;
}

YGWrap toYoga(layout::Wrap value) {
    switch (value) {
        case layout::Wrap::NoWrap: return YGWrapNoWrap;
        case layout::Wrap::Wrap: return YGWrapWrap;
        case layout::Wrap::WrapReverse: return YGWrapWrapReverse;
    }
    return YGWrapNoWrap;
}

YGEdge toYoga(layout::Edge value) {
    switch (value) {
        case layout::Edge::Left: return YGEdgeLeft;
        case layout::Edge::Top: return YGEdgeTop;
        case layout::Edge::Right: return YGEdgeRight;
        case layout::Edge::Bottom: return YGEdgeBottom;
        case layout::Edge::Start: return YGEdgeStart;
        case layout::Edge::End: return YGEdgeEnd;
        case layout::Edge::Horizontal: return YGEdgeHorizontal;
        case layout::Edge::Vertical: return YGEdgeVertical;
        case layout::Edge::All: return YGEdgeAll;
    }
    return YGEdgeAll;
}

YGGutter toYoga(layout::Gutter value) {
    switch (value) {
        case layout::Gutter::Column: return YGGutterColumn;
        case layout::Gutter::Row: return YGGutterRow;
        case layout::Gutter::All: return YGGutterAll;
    }
    return YGGutterAll;
}

layout::MeasureMode fromYoga(YGMeasureMode value) {
    switch (value) {
        case YGMeasureModeUndefined: return layout::MeasureMode::Undefined;
        case YGMeasureModeExactly: return layout::MeasureMode::Exactly;
        case YGMeasureModeAtMost: return layout::MeasureMode::AtMost;
    }
    return layout::MeasureMode::Undefined;
}

} // namespace

struct LayoutNode::Impl {
    Impl() : node(YGNodeNew()) { YGNodeSetContext(node, this); }
    ~Impl() { if (node) YGNodeFree(node); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    template <typename Mutation>
    void mutate(Mutation&& mutation) {
        if (!node) return;
        const bool wasDirty = YGNodeIsDirty(node);
        std::forward<Mutation>(mutation)();
        if (!wasDirty && YGNodeIsDirty(node) && invalidationCallback) {
            invalidationCallback();
        }
    }

    static YGSize measure(YGNodeRef node, float width, YGMeasureMode widthMode,
                          float height, YGMeasureMode heightMode) {
        auto* self = node ? static_cast<Impl*>(YGNodeGetContext(node)) : nullptr;
        if (!self || !self->measureCallback) return {0.0f, 0.0f};
        const layout::Size size = self->measureCallback({
            {width, fromYoga(widthMode)},
            {height, fromYoga(heightMode)},
        });
        return {size.width, size.height};
    }

    YGNodeRef node{nullptr};
    MeasureCallback measureCallback;
    InvalidationCallback invalidationCallback;
};

LayoutNode::LayoutNode() : m_impl(std::make_unique<Impl>()) {}
LayoutNode::~LayoutNode() = default;
LayoutNode::LayoutNode(LayoutNode&&) noexcept = default;
LayoutNode& LayoutNode::operator=(LayoutNode&&) noexcept = default;

void LayoutNode::setDirection(layout::Direction value) { m_impl->mutate([&] { YGNodeStyleSetFlexDirection(m_impl->node, toYoga(value)); }); }
void LayoutNode::setJustifyContent(layout::Justify value) { m_impl->mutate([&] { YGNodeStyleSetJustifyContent(m_impl->node, toYoga(value)); }); }
void LayoutNode::setAlignItems(layout::Align value) { m_impl->mutate([&] { YGNodeStyleSetAlignItems(m_impl->node, toYoga(value)); }); }
void LayoutNode::setAlignSelf(layout::Align value) { m_impl->mutate([&] { YGNodeStyleSetAlignSelf(m_impl->node, toYoga(value)); }); }
void LayoutNode::setPositionType(layout::PositionType value) { m_impl->mutate([&] { YGNodeStyleSetPositionType(m_impl->node, toYoga(value)); }); }
layout::PositionType LayoutNode::positionType() const { return fromYoga(YGNodeStyleGetPositionType(m_impl->node)); }
void LayoutNode::setWrap(layout::Wrap value) { m_impl->mutate([&] { YGNodeStyleSetFlexWrap(m_impl->node, toYoga(value)); }); }
void LayoutNode::setFlexGrow(float value) { m_impl->mutate([&] { YGNodeStyleSetFlexGrow(m_impl->node, value); }); }
void LayoutNode::setFlexShrink(float value) { m_impl->mutate([&] { YGNodeStyleSetFlexShrink(m_impl->node, value); }); }
void LayoutNode::setFlexBasis(float value) { m_impl->mutate([&] { YGNodeStyleSetFlexBasis(m_impl->node, value); }); }
void LayoutNode::setFlexBasisAuto() { m_impl->mutate([&] { YGNodeStyleSetFlexBasisAuto(m_impl->node); }); }
void LayoutNode::setWidth(float value) { m_impl->mutate([&] { YGNodeStyleSetWidth(m_impl->node, value); }); }
void LayoutNode::setWidthAuto() { m_impl->mutate([&] { YGNodeStyleSetWidthAuto(m_impl->node); }); }
void LayoutNode::setHeight(float value) { m_impl->mutate([&] { YGNodeStyleSetHeight(m_impl->node, value); }); }
void LayoutNode::setHeightAuto() { m_impl->mutate([&] { YGNodeStyleSetHeightAuto(m_impl->node); }); }
void LayoutNode::setMinWidth(float value) { m_impl->mutate([&] { YGNodeStyleSetMinWidth(m_impl->node, value); }); }
void LayoutNode::setMinHeight(float value) { m_impl->mutate([&] { YGNodeStyleSetMinHeight(m_impl->node, value); }); }
void LayoutNode::setMaxWidth(float value) { m_impl->mutate([&] { YGNodeStyleSetMaxWidth(m_impl->node, value); }); }
void LayoutNode::setMaxHeight(float value) { m_impl->mutate([&] { YGNodeStyleSetMaxHeight(m_impl->node, value); }); }
void LayoutNode::setPadding(layout::Edge edge, float value) { m_impl->mutate([&] { YGNodeStyleSetPadding(m_impl->node, toYoga(edge), value); }); }
void LayoutNode::setMargin(layout::Edge edge, float value) { m_impl->mutate([&] { YGNodeStyleSetMargin(m_impl->node, toYoga(edge), value); }); }
void LayoutNode::setGap(layout::Gutter gutter, float value) { m_impl->mutate([&] { YGNodeStyleSetGap(m_impl->node, toYoga(gutter), value); }); }
void LayoutNode::setPosition(layout::Edge edge, float value) { m_impl->mutate([&] { YGNodeStyleSetPosition(m_impl->node, toYoga(edge), value); }); }
void LayoutNode::setCollapsed(bool collapsed) {
    m_impl->mutate([&] {
        YGNodeStyleSetDisplay(m_impl->node, collapsed ? YGDisplayNone : YGDisplayFlex);
    });
}

void LayoutNode::appendChild(LayoutNode& child) {
    YGNodeInsertChild(m_impl->node, child.m_impl->node, YGNodeGetChildCount(m_impl->node));
    if (m_impl->invalidationCallback) m_impl->invalidationCallback();
}

void LayoutNode::removeChild(LayoutNode& child) {
    YGNodeRemoveChild(m_impl->node, child.m_impl->node);
    if (m_impl->invalidationCallback) m_impl->invalidationCallback();
}

void LayoutNode::calculateLayout(float width, float height) {
    YGNodeCalculateLayout(m_impl->node, width, height, YGDirectionLTR);
}

float LayoutNode::layoutX() const { return YGNodeLayoutGetLeft(m_impl->node); }
float LayoutNode::layoutY() const { return YGNodeLayoutGetTop(m_impl->node); }
float LayoutNode::layoutWidth() const { return YGNodeLayoutGetWidth(m_impl->node); }
float LayoutNode::layoutHeight() const { return YGNodeLayoutGetHeight(m_impl->node); }

void LayoutNode::setMeasureCallback(MeasureCallback callback) {
    m_impl->measureCallback = std::move(callback);
    YGNodeSetMeasureFunc(m_impl->node,
                         m_impl->measureCallback ? &Impl::measure : nullptr);
    if (m_impl->invalidationCallback) m_impl->invalidationCallback();
}

void LayoutNode::invalidateMeasurement() {
    YGNodeMarkDirty(m_impl->node);
    if (m_impl->invalidationCallback) m_impl->invalidationCallback();
}

void LayoutNode::setInvalidationCallback(InvalidationCallback callback) {
    m_impl->invalidationCallback = std::move(callback);
}

} // namespace lcl::ui::detail
