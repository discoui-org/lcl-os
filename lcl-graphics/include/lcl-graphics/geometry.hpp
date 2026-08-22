#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace lcl::graphics {

struct PointF {
    float x{0.0f};
    float y{0.0f};
};

struct SizeF {
    float width{0.0f};
    float height{0.0f};
};

struct PixelSize {
    uint32_t width{0};
    uint32_t height{0};
};

struct RectF {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};

    constexpr bool isEmpty() const { return width <= 0.0f || height <= 0.0f; }
    constexpr bool containsPoint(float px, float py) const {
        return px >= x && px <= x + width && py >= y && py <= y + height;
    }
    constexpr bool intersects(const RectF& other) const {
        return !isEmpty() && !other.isEmpty() &&
               !(x + width <= other.x || other.x + other.width <= x ||
                 y + height <= other.y || other.y + other.height <= y);
    }
    RectF intersection(const RectF& other) const;
    RectF unionWith(const RectF& other) const;
};

struct RRect {
    RectF bounds{};
    float radiusX{0.0f};
    float radiusY{0.0f};
    float roundness{2.0f};
};

struct Matrix3 {
    float a{1.0f};
    float b{0.0f};
    float c{0.0f};
    float d{1.0f};
    float tx{0.0f};
    float ty{0.0f};

    static constexpr Matrix3 identity() { return {}; }
    static constexpr Matrix3 translation(float x, float y) {
        return {1.0f, 0.0f, 0.0f, 1.0f, x, y};
    }
    static constexpr Matrix3 scale(float x, float y) {
        return {x, 0.0f, 0.0f, y, 0.0f, 0.0f};
    }

    PointF mapPoint(PointF point) const {
        return {a * point.x + c * point.y + tx,
                b * point.x + d * point.y + ty};
    }
    RectF mapRect(const RectF& rect) const;
    Matrix3 followedBy(const Matrix3& next) const;
    std::optional<Matrix3> inverted() const;
    float maxScale() const {
        return std::max(std::hypot(a, b), std::hypot(c, d));
    }
};

struct RenderTarget {
    SizeF logicalSize{};
    PixelSize pixelSize{};
    float deviceScale{1.0f};
};

inline RectF RectF::intersection(const RectF& other) const {
    if (!intersects(other)) return {};
    const float left = std::max(x, other.x);
    const float top = std::max(y, other.y);
    const float right = std::min(x + width, other.x + other.width);
    const float bottom = std::min(y + height, other.y + other.height);
    return {left, top, right - left, bottom - top};
}

inline RectF RectF::unionWith(const RectF& other) const {
    if (isEmpty()) return other;
    if (other.isEmpty()) return *this;
    const float left = std::min(x, other.x);
    const float top = std::min(y, other.y);
    const float right = std::max(x + width, other.x + other.width);
    const float bottom = std::max(y + height, other.y + other.height);
    return {left, top, right - left, bottom - top};
}

inline RectF Matrix3::mapRect(const RectF& rect) const {
    const auto p0 = mapPoint({rect.x, rect.y});
    const auto p1 = mapPoint({rect.x + rect.width, rect.y});
    const auto p2 = mapPoint({rect.x, rect.y + rect.height});
    const auto p3 = mapPoint({rect.x + rect.width, rect.y + rect.height});
    const float left = std::min({p0.x, p1.x, p2.x, p3.x});
    const float top = std::min({p0.y, p1.y, p2.y, p3.y});
    const float right = std::max({p0.x, p1.x, p2.x, p3.x});
    const float bottom = std::max({p0.y, p1.y, p2.y, p3.y});
    return {left, top, right - left, bottom - top};
}

inline Matrix3 Matrix3::followedBy(const Matrix3& next) const {
    return {
        next.a * a + next.c * b,
        next.b * a + next.d * b,
        next.a * c + next.c * d,
        next.b * c + next.d * d,
        next.a * tx + next.c * ty + next.tx,
        next.b * tx + next.d * ty + next.ty,
    };
}

inline std::optional<Matrix3> Matrix3::inverted() const {
    const float determinant = a * d - b * c;
    if (std::fabs(determinant) <= 1.0e-8f) return std::nullopt;
    const float inv = 1.0f / determinant;
    return Matrix3{d * inv, -b * inv, -c * inv, a * inv,
                   (c * ty - d * tx) * inv, (b * tx - a * ty) * inv};
}

inline RectF enclosingDeviceRect(const RectF& logical, const Matrix3& transform) {
    const RectF mapped = transform.mapRect(logical);
    const float left = std::floor(mapped.x);
    const float top = std::floor(mapped.y);
    const float right = std::ceil(mapped.x + mapped.width);
    const float bottom = std::ceil(mapped.y + mapped.height);
    return {left, top, right - left, bottom - top};
}

} // namespace lcl::graphics
