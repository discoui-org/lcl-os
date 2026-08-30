#pragma once

#include <cstdint>
#include <vector>

#include "lcl-graphics/geometry.hpp"
#include "lcl-graphics/paint.hpp"

namespace lcl::graphics {

enum class PathVerb : uint8_t { MoveTo, LineTo, QuadTo, CubicTo, Arc, Close };

enum class PathPrimitiveKind : uint8_t { Rect, RRect, Ellipse, TopRRect };

struct PathPrimitive {
    PathPrimitiveKind kind{PathPrimitiveKind::Rect};
    RectF bounds{};
    float radiusX{0.0f};
    float radiusY{0.0f};
    float roundness{2.0f};
};

struct PathElement {
    PathVerb verb{PathVerb::MoveTo};
    PointF p0{};
    PointF p1{};
    PointF p2{};
    float radiusX{0.0f};
    float radiusY{0.0f};
    float rotationRadians{0.0f};
    bool largeArc{false};
    bool clockwise{true};
};

class Path {
public:
    Path& moveTo(float x, float y);
    Path& lineTo(float x, float y);
    Path& quadTo(float cx, float cy, float x, float y);
    Path& cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y);
    Path& arcTo(float radiusX, float radiusY, float rotationRadians,
                bool largeArc, bool clockwise, float x, float y);
    Path& close();
    Path& addRect(const RectF& rect);
    Path& addRRect(const RRect& rect);
    Path& addEllipse(const RectF& bounds);
    Path& addTopRRect(const RectF& bounds, float radius,
                      float roundness = 2.0f);

    const std::vector<PathElement>& elements() const noexcept { return m_elements; }
    const PathPrimitive* primitive() const noexcept {
        return m_hasPrimitive ? &m_primitive : nullptr;
    }
    bool empty() const noexcept { return m_elements.empty(); }
    void clear() { m_elements.clear(); m_hasPrimitive = false; }

private:
    void invalidatePrimitive() noexcept { m_hasPrimitive = false; }

    std::vector<PathElement> m_elements;
    PathPrimitive m_primitive{};
    bool m_hasPrimitive{false};
};

struct FlattenedContour {
    std::vector<PointF> points;
    bool closed{false};
};

std::vector<FlattenedContour> flattenPath(const Path& path,
                                          const Matrix3& transform,
                                          float deviceTolerance = 0.25f);

} // namespace lcl::graphics
