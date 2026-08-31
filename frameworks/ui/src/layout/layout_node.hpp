#pragma once

#include "lcl-ui/layout/layout.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace lcl::ui::detail {

class LayoutNode {
public:
    using MeasureCallback =
        std::function<layout::Size(const layout::Constraints&)>;
    using InvalidationCallback = std::function<void()>;

    LayoutNode();
    ~LayoutNode();

    LayoutNode(const LayoutNode&) = delete;
    LayoutNode& operator=(const LayoutNode&) = delete;
    LayoutNode(LayoutNode&&) noexcept;
    LayoutNode& operator=(LayoutNode&&) noexcept;

    void setDirection(layout::Direction direction);
    void setJustifyContent(layout::Justify justify);
    void setAlignItems(layout::Align align);
    void setAlignSelf(layout::Align align);
    void setPositionType(layout::PositionType type);
    layout::PositionType positionType() const;
    void setWrap(layout::Wrap wrap);
    void setFlexGrow(float value);
    void setFlexShrink(float value);
    void setFlexBasis(float value);
    void setFlexBasisAuto();
    void setWidth(float value);
    void setWidthAuto();
    void setHeight(float value);
    void setHeightAuto();
    void setMinWidth(float value);
    void setMinHeight(float value);
    void setMaxWidth(float value);
    void setMaxHeight(float value);
    void setPadding(layout::Edge edge, float value);
    void setMargin(layout::Edge edge, float value);
    void setGap(layout::Gutter gutter, float value);
    void setPosition(layout::Edge edge, float value);
    /** Removes this node from layout without exposing Yoga to callers. */
    void setCollapsed(bool collapsed);

    void appendChild(LayoutNode& child);
    void removeChild(LayoutNode& child);
    void calculateLayout(float availableWidth, float availableHeight);
    float layoutX() const;
    float layoutY() const;
    float layoutWidth() const;
    float layoutHeight() const;

    void setMeasureCallback(MeasureCallback callback);
    void invalidateMeasurement();
    void setInvalidationCallback(InvalidationCallback callback);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace lcl::ui::detail
