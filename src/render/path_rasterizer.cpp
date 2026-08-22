#include "render/path_rasterizer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lcl::render {

RasterizedPath rasterizePath(const lcl::graphics::Path& path,
                             const lcl::graphics::Paint& paint,
                             const lcl::graphics::Matrix3& deviceTransform,
                             float inheritedOpacity) {
    RasterizedPath result{};
    if (path.empty() || paint.color.a == 0 || paint.opacity <= 0.0f ||
        inheritedOpacity <= 0.0f) return result;

    const auto contours = lcl::graphics::flattenPath(path, deviceTransform);
    if (contours.empty()) return result;

    float minX = std::numeric_limits<float>::infinity();
    float minY = std::numeric_limits<float>::infinity();
    float maxX = -std::numeric_limits<float>::infinity();
    float maxY = -std::numeric_limits<float>::infinity();
    for (const auto& contour : contours) {
        for (const auto& point : contour.points) {
            minX = std::min(minX, point.x);
            minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x);
            maxY = std::max(maxY, point.y);
        }
    }
    if (!std::isfinite(minX) || maxX <= minX || maxY <= minY) return result;

    const float strokeWidth = paint.stroke.scaling == lcl::graphics::StrokeScaling::Hairline
        ? 1.0f
        : std::max(0.0f, paint.stroke.width * deviceTransform.maxScale());
    const float outset = paint.style == lcl::graphics::PaintStyle::Stroke
        ? strokeWidth * 0.5f + 1.0f : 1.0f;
    result.x = static_cast<int>(std::floor(minX - outset));
    result.y = static_cast<int>(std::floor(minY - outset));
    const int right = static_cast<int>(std::ceil(maxX + outset));
    const int bottom = static_cast<int>(std::ceil(maxY + outset));
    result.width = std::max(1, right - result.x);
    result.height = std::max(1, bottom - result.y);
    result.pixels.assign(static_cast<size_t>(result.width) * result.height, 0u);

    const auto fillContains = [&](float px, float py) {
        int winding = 0;
        bool odd = false;
        for (const auto& contour : contours) {
            if (contour.points.size() < 3) continue;
            for (size_t i = 1; i < contour.points.size(); ++i) {
                const auto& a = contour.points[i - 1];
                const auto& b = contour.points[i];
                const bool crosses = (a.y <= py && b.y > py) || (a.y > py && b.y <= py);
                if (!crosses) continue;
                const float x = a.x + (py - a.y) * (b.x - a.x) / (b.y - a.y);
                if (x <= px) continue;
                odd = !odd;
                winding += b.y > a.y ? 1 : -1;
            }
        }
        return paint.fillRule == lcl::graphics::FillRule::EvenOdd ? odd : winding != 0;
    };

    const auto pointInTriangle = [](float px, float py,
                                    const lcl::graphics::PointF& a,
                                    const lcl::graphics::PointF& b,
                                    const lcl::graphics::PointF& c) {
        const auto cross = [](const lcl::graphics::PointF& p0,
                              const lcl::graphics::PointF& p1,
                              float x, float y) {
            return (p1.x - p0.x) * (y - p0.y) -
                   (p1.y - p0.y) * (x - p0.x);
        };
        const float c0 = cross(a, b, px, py);
        const float c1 = cross(b, c, px, py);
        const float c2 = cross(c, a, px, py);
        return (c0 >= 0.0f && c1 >= 0.0f && c2 >= 0.0f) ||
               (c0 <= 0.0f && c1 <= 0.0f && c2 <= 0.0f);
    };

    const auto strokeContains = [&](float px, float py) {
        const float maxDistanceSq = strokeWidth * strokeWidth * 0.25f;
        const float halfWidth = strokeWidth * 0.5f;
        for (const auto& contour : contours) {
            for (size_t i = 1; i < contour.points.size(); ++i) {
                const auto& a = contour.points[i - 1];
                const auto& b = contour.points[i];
                const float vx = b.x - a.x;
                const float vy = b.y - a.y;
                const float lengthSq = vx * vx + vy * vy;
                if (lengthSq <= 1.0e-8f) continue;
                const float length = std::sqrt(lengthSq);
                const float rawT = ((px - a.x) * vx + (py - a.y) * vy) / lengthSq;
                float minT = 0.0f;
                float maxT = 1.0f;
                const bool first = i == 1;
                const bool last = i + 1 == contour.points.size();
                if (!contour.closed && paint.stroke.cap == lcl::graphics::StrokeCap::Square) {
                    if (first) minT = -halfWidth / length;
                    if (last) maxT = 1.0f + halfWidth / length;
                }
                if (rawT < minT || rawT > maxT) continue;
                const float t = std::clamp(rawT, minT, maxT);
                const float dx = px - (a.x + vx * t);
                const float dy = py - (a.y + vy * t);
                if (dx * dx + dy * dy <= maxDistanceSq) return true;
            }

            if (!contour.closed &&
                paint.stroke.cap == lcl::graphics::StrokeCap::Round &&
                !contour.points.empty()) {
                const auto& first = contour.points.front();
                const auto& last = contour.points.back();
                const float firstDx = px - first.x;
                const float firstDy = py - first.y;
                const float lastDx = px - last.x;
                const float lastDy = py - last.y;
                if (firstDx * firstDx + firstDy * firstDy <= maxDistanceSq ||
                    lastDx * lastDx + lastDy * lastDy <= maxDistanceSq) {
                    return true;
                }
            }

            const size_t pointCount = contour.points.size();
            if (pointCount < 3) continue;
            const size_t firstJoin = contour.closed ? 0 : 1;
            const size_t lastJoin = contour.closed ? pointCount - 1 : pointCount - 2;
            for (size_t index = firstJoin; index <= lastJoin; ++index) {
                const size_t previous = index == 0 ? pointCount - 2 : index - 1;
                const size_t next = index + 1 >= pointCount ? 1 : index + 1;
                const auto& a = contour.points[previous];
                const auto& b = contour.points[index];
                const auto& c = contour.points[next];
                if (paint.stroke.join == lcl::graphics::StrokeJoin::Round) {
                    const float dx = px - b.x;
                    const float dy = py - b.y;
                    if (dx * dx + dy * dy <= maxDistanceSq) return true;
                    continue;
                }

                const float d1x = b.x - a.x;
                const float d1y = b.y - a.y;
                const float d2x = c.x - b.x;
                const float d2y = c.y - b.y;
                const float l1 = std::hypot(d1x, d1y);
                const float l2 = std::hypot(d2x, d2y);
                if (l1 <= 1.0e-6f || l2 <= 1.0e-6f) continue;
                for (float side : {-1.0f, 1.0f}) {
                    const lcl::graphics::PointF outer1{
                        b.x - d1y / l1 * halfWidth * side,
                        b.y + d1x / l1 * halfWidth * side};
                    const lcl::graphics::PointF outer2{
                        b.x - d2y / l2 * halfWidth * side,
                        b.y + d2x / l2 * halfWidth * side};
                    lcl::graphics::PointF joinPoint = b;
                    if (paint.stroke.join == lcl::graphics::StrokeJoin::Miter) {
                        const float determinant = d1x * d2y - d1y * d2x;
                        if (std::abs(determinant) > 1.0e-6f) {
                            const float qx = outer2.x - outer1.x;
                            const float qy = outer2.y - outer1.y;
                            const float t = (qx * d2y - qy * d2x) / determinant;
                            const lcl::graphics::PointF candidate{
                                outer1.x + d1x * t, outer1.y + d1y * t};
                            if (std::hypot(candidate.x - b.x, candidate.y - b.y) <=
                                std::max(1.0f, paint.stroke.miterLimit) * halfWidth) {
                                joinPoint = candidate;
                            }
                        }
                    }
                    if (pointInTriangle(px, py, outer1, joinPoint, outer2)) {
                        return true;
                    }
                }
            }
        }
        return false;
    };

    constexpr int sampleAxis = 4;
    constexpr float invSamples = 1.0f / static_cast<float>(sampleAxis * sampleAxis);
    const float opacity = std::clamp(paint.opacity * inheritedOpacity, 0.0f, 1.0f);
    for (int y = 0; y < result.height; ++y) {
        for (int x = 0; x < result.width; ++x) {
            int covered = 0;
            for (int sy = 0; sy < sampleAxis; ++sy) {
                for (int sx = 0; sx < sampleAxis; ++sx) {
                    const float px = static_cast<float>(result.x + x) +
                        (static_cast<float>(sx) + 0.5f) / sampleAxis;
                    const float py = static_cast<float>(result.y + y) +
                        (static_cast<float>(sy) + 0.5f) / sampleAxis;
                    covered += paint.style == lcl::graphics::PaintStyle::Fill
                        ? fillContains(px, py) : strokeContains(px, py);
                }
            }
            if (covered == 0) continue;
            const float coverage = static_cast<float>(covered) * invSamples * opacity;
            const uint32_t alpha = static_cast<uint32_t>(std::clamp(
                std::lround(static_cast<float>(paint.color.a) * coverage), 0l, 255l));
            result.pixels[static_cast<size_t>(y) * result.width + x] =
                (alpha << 24) | (static_cast<uint32_t>(paint.color.r) << 16) |
                (static_cast<uint32_t>(paint.color.g) << 8) | paint.color.b;
        }
    }
    return result;
}

} // namespace lcl::render
