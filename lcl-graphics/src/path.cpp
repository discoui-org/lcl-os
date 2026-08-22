#include "lcl-graphics/path.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lcl::graphics {

Path& Path::moveTo(float x, float y) {
    invalidatePrimitive();
    m_elements.push_back({PathVerb::MoveTo, {x, y}});
    return *this;
}
Path& Path::lineTo(float x, float y) {
    invalidatePrimitive();
    m_elements.push_back({PathVerb::LineTo, {x, y}});
    return *this;
}
Path& Path::quadTo(float cx, float cy, float x, float y) {
    invalidatePrimitive();
    m_elements.push_back({PathVerb::QuadTo, {cx, cy}, {x, y}});
    return *this;
}
Path& Path::cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y) {
    invalidatePrimitive();
    m_elements.push_back({PathVerb::CubicTo, {c1x, c1y}, {c2x, c2y}, {x, y}});
    return *this;
}
Path& Path::arcTo(float radiusX, float radiusY, float rotationRadians,
                  bool largeArc, bool clockwise, float x, float y) {
    invalidatePrimitive();
    PathElement element{};
    element.verb = PathVerb::Arc;
    element.p0 = {x, y};
    element.radiusX = radiusX;
    element.radiusY = radiusY;
    element.rotationRadians = rotationRadians;
    element.largeArc = largeArc;
    element.clockwise = clockwise;
    m_elements.push_back(element);
    return *this;
}
Path& Path::close() {
    invalidatePrimitive();
    m_elements.push_back({PathVerb::Close});
    return *this;
}
Path& Path::addRect(const RectF& rect) {
    const bool wasEmpty = m_elements.empty();
    moveTo(rect.x, rect.y).lineTo(rect.x + rect.width, rect.y)
        .lineTo(rect.x + rect.width, rect.y + rect.height)
        .lineTo(rect.x, rect.y + rect.height).close();
    if (wasEmpty) {
        m_primitive = {PathPrimitiveKind::Rect, rect, 0.0f, 0.0f, 2.0f};
        m_hasPrimitive = true;
    }
    return *this;
}
Path& Path::addRRect(const RRect& rrect) {
    const bool wasEmpty = m_elements.empty();
    const auto& r = rrect.bounds;
    const float rx = std::clamp(rrect.radiusX, 0.0f, r.width * 0.5f);
    const float ry = std::clamp(rrect.radiusY, 0.0f, r.height * 0.5f);
    if (rx <= 0.0f || ry <= 0.0f) return addRect(r);
    const float exponent = std::clamp(rrect.roundness, 2.0f, 8.0f);
    constexpr int cornerSegments = 12;
    const auto appendCorner = [&](float centerX, float centerY,
                                  float startAngle, float endAngle) {
        for (int index = 1; index <= cornerSegments; ++index) {
            const float t = static_cast<float>(index) / cornerSegments;
            const float angle = startAngle + (endAngle - startAngle) * t;
            const float cosine = std::cos(angle);
            const float sine = std::sin(angle);
            const float x = centerX + rx * std::copysign(
                std::pow(std::abs(cosine), 2.0f / exponent), cosine);
            const float y = centerY + ry * std::copysign(
                std::pow(std::abs(sine), 2.0f / exponent), sine);
            lineTo(x, y);
        }
    };
    constexpr float halfPi = std::numbers::pi_v<float> * 0.5f;
    moveTo(r.x + rx, r.y).lineTo(r.x + r.width - rx, r.y);
    appendCorner(r.x + r.width - rx, r.y + ry, -halfPi, 0.0f);
    lineTo(r.x + r.width, r.y + r.height - ry);
    appendCorner(r.x + r.width - rx, r.y + r.height - ry, 0.0f, halfPi);
    lineTo(r.x + rx, r.y + r.height);
    appendCorner(r.x + rx, r.y + r.height - ry, halfPi, halfPi * 2.0f);
    lineTo(r.x, r.y + ry);
    appendCorner(r.x + rx, r.y + ry, halfPi * 2.0f, halfPi * 3.0f);
    close();
    if (wasEmpty) {
        m_primitive = {PathPrimitiveKind::RRect, r, rx, ry, exponent};
        m_hasPrimitive = true;
    }
    return *this;
}
Path& Path::addEllipse(const RectF& bounds) {
    const bool wasEmpty = m_elements.empty();
    addRRect({bounds, bounds.width * 0.5f, bounds.height * 0.5f, 2.0f});
    if (wasEmpty) {
        m_primitive = {PathPrimitiveKind::Ellipse, bounds,
                       bounds.width * 0.5f, bounds.height * 0.5f, 2.0f};
        m_hasPrimitive = true;
    }
    return *this;
}
Path& Path::addTopRRect(const RectF& bounds, float radius, float roundness) {
    const bool wasEmpty = m_elements.empty();
    const float r = std::clamp(radius, 0.0f,
                               std::min(bounds.width * 0.5f, bounds.height));
    addRRect({bounds, r, r, roundness});
    addRect({bounds.x, bounds.y + r, bounds.width,
             std::max(0.0f, bounds.height - r)});
    if (wasEmpty) {
        m_primitive = {PathPrimitiveKind::TopRRect, bounds, r, r,
                       std::clamp(roundness, 2.0f, 8.0f)};
        m_hasPrimitive = true;
    }
    return *this;
}

namespace {
PointF lerp(PointF a, PointF b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}
int curveSegments(PointF a, PointF b, PointF c, PointF d, float tolerance) {
    const float estimate = std::hypot(b.x - a.x, b.y - a.y) +
                           std::hypot(c.x - b.x, c.y - b.y) +
                           std::hypot(d.x - c.x, d.y - c.y);
    return std::clamp(static_cast<int>(std::ceil(estimate / std::max(0.05f, tolerance * 4.0f))), 2, 128);
}

float vectorAngle(float ux, float uy, float vx, float vy) {
    const float dot = ux * vx + uy * vy;
    const float det = ux * vy - uy * vx;
    return std::atan2(det, dot);
}

void appendArc(std::vector<PointF>& points, PointF start,
               const PathElement& arc, const Matrix3& transform,
               float tolerance) {
    PointF end = arc.p0;
    float rx = std::abs(arc.radiusX);
    float ry = std::abs(arc.radiusY);
    if (rx <= 1.0e-6f || ry <= 1.0e-6f ||
        (std::abs(start.x - end.x) <= 1.0e-6f &&
         std::abs(start.y - end.y) <= 1.0e-6f)) {
        points.push_back(transform.mapPoint(end));
        return;
    }

    const float phi = arc.rotationRadians;
    const float cosPhi = std::cos(phi);
    const float sinPhi = std::sin(phi);
    const float halfDx = (start.x - end.x) * 0.5f;
    const float halfDy = (start.y - end.y) * 0.5f;
    const float x1p = cosPhi * halfDx + sinPhi * halfDy;
    const float y1p = -sinPhi * halfDx + cosPhi * halfDy;

    float rx2 = rx * rx;
    float ry2 = ry * ry;
    const float radiiScale = x1p * x1p / rx2 + y1p * y1p / ry2;
    if (radiiScale > 1.0f) {
        const float grow = std::sqrt(radiiScale);
        rx *= grow;
        ry *= grow;
        rx2 = rx * rx;
        ry2 = ry * ry;
    }

    const float numerator = std::max(
        0.0f, rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p);
    const float denominator = std::max(
        1.0e-12f, rx2 * y1p * y1p + ry2 * x1p * x1p);
    const float sign = arc.largeArc == arc.clockwise ? -1.0f : 1.0f;
    const float coefficient = sign * std::sqrt(numerator / denominator);
    const float cxp = coefficient * rx * y1p / ry;
    const float cyp = coefficient * -ry * x1p / rx;
    const float centerX = cosPhi * cxp - sinPhi * cyp +
                          (start.x + end.x) * 0.5f;
    const float centerY = sinPhi * cxp + cosPhi * cyp +
                          (start.y + end.y) * 0.5f;

    const float ux = (x1p - cxp) / rx;
    const float uy = (y1p - cyp) / ry;
    const float vx = (-x1p - cxp) / rx;
    const float vy = (-y1p - cyp) / ry;
    float theta = vectorAngle(1.0f, 0.0f, ux, uy);
    float sweep = vectorAngle(ux, uy, vx, vy);
    constexpr float tau = 2.0f * std::numbers::pi_v<float>;
    if (!arc.clockwise && sweep > 0.0f) sweep -= tau;
    if (arc.clockwise && sweep < 0.0f) sweep += tau;

    const float estimatedLength = std::abs(sweep) * std::max(rx, ry);
    const int segments = std::clamp(
        static_cast<int>(std::ceil(estimatedLength /
            std::max(0.05f, tolerance * 4.0f))), 2, 256);
    for (int index = 1; index <= segments; ++index) {
        const float angle = theta + sweep *
            (static_cast<float>(index) / static_cast<float>(segments));
        const float ellipseX = rx * std::cos(angle);
        const float ellipseY = ry * std::sin(angle);
        points.push_back(transform.mapPoint({
            centerX + cosPhi * ellipseX - sinPhi * ellipseY,
            centerY + sinPhi * ellipseX + cosPhi * ellipseY,
        }));
    }
}
}

std::vector<FlattenedContour> flattenPath(const Path& path,
                                          const Matrix3& transform,
                                          float deviceTolerance) {
    std::vector<FlattenedContour> contours;
    FlattenedContour* contour = nullptr;
    PointF current{};
    PointF start{};
    const float localTolerance = deviceTolerance / std::max(0.001f, transform.maxScale());

    auto ensureContour = [&]() -> FlattenedContour& {
        if (!contour) {
            contours.push_back({});
            contour = &contours.back();
            contour->points.push_back(transform.mapPoint(current));
        }
        return *contour;
    };

    for (const auto& element : path.elements()) {
        switch (element.verb) {
            case PathVerb::MoveTo:
                current = start = element.p0;
                contours.push_back({});
                contour = &contours.back();
                contour->points.push_back(transform.mapPoint(current));
                break;
            case PathVerb::LineTo:
                ensureContour().points.push_back(transform.mapPoint(element.p0));
                current = element.p0;
                break;
            case PathVerb::QuadTo: {
                auto& points = ensureContour().points;
                const int segments = curveSegments(current, element.p0, element.p0, element.p1, localTolerance);
                for (int i = 1; i <= segments; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(segments);
                    const PointF ab = lerp(current, element.p0, t);
                    const PointF bc = lerp(element.p0, element.p1, t);
                    points.push_back(transform.mapPoint(lerp(ab, bc, t)));
                }
                current = element.p1;
                break;
            }
            case PathVerb::CubicTo: {
                auto& points = ensureContour().points;
                const int segments = curveSegments(current, element.p0, element.p1, element.p2, localTolerance);
                for (int i = 1; i <= segments; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(segments);
                    const PointF ab = lerp(current, element.p0, t);
                    const PointF bc = lerp(element.p0, element.p1, t);
                    const PointF cd = lerp(element.p1, element.p2, t);
                    points.push_back(transform.mapPoint(lerp(lerp(ab, bc, t), lerp(bc, cd, t), t)));
                }
                current = element.p2;
                break;
            }
            case PathVerb::Arc:
                appendArc(ensureContour().points, current, element,
                          transform, localTolerance);
                current = element.p0;
                break;
            case PathVerb::Close:
                if (contour) {
                    contour->closed = true;
                    if (!contour->points.empty() &&
                        (contour->points.back().x != contour->points.front().x ||
                         contour->points.back().y != contour->points.front().y)) {
                        contour->points.push_back(contour->points.front());
                    }
                }
                current = start;
                contour = nullptr;
                break;
        }
    }
    return contours;
}

} // namespace lcl::graphics
