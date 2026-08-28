#pragma once

namespace lcl::ui::layout {

enum class Direction {
    Row,
    RowReverse,
    Column,
    ColumnReverse,
};

enum class Justify {
    FlexStart,
    Center,
    FlexEnd,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly,
};

enum class Align {
    Auto,
    FlexStart,
    Center,
    FlexEnd,
    Stretch,
    Baseline,
    SpaceBetween,
    SpaceAround,
};

enum class PositionType {
    Static,
    Relative,
    Absolute,
};

enum class Wrap {
    NoWrap,
    Wrap,
    WrapReverse,
};

enum class Edge {
    Left,
    Top,
    Right,
    Bottom,
    Start,
    End,
    Horizontal,
    Vertical,
    All,
};

enum class Gutter {
    Column,
    Row,
    All,
};

enum class MeasureMode {
    Undefined,
    Exactly,
    AtMost,
};

struct MeasureConstraint {
    float value{0.0f};
    MeasureMode mode{MeasureMode::Undefined};
};

struct Constraints {
    MeasureConstraint width{};
    MeasureConstraint height{};
};

struct Size {
    float width{0.0f};
    float height{0.0f};
};

} // namespace lcl::ui::layout
