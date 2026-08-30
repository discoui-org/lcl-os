#pragma once

#include <algorithm>
#include <cstdint>

namespace lcl::render {

struct Rect {
    int x{0};
    int y{0};
    int width{0};
    int height{0};

    constexpr bool empty() const {
        return width <= 0 || height <= 0;
    }

    constexpr bool intersects(const Rect& other) const {
        if (empty() || other.empty()) return false;
        return !(x + width <= other.x ||
                 other.x + other.width <= x ||
                 y + height <= other.y ||
                 other.y + other.height <= y);
    }

    constexpr bool contains(const Rect& other) const {
        if (empty() || other.empty()) return false;
        return (other.x >= x &&
                other.y >= y &&
                other.x + other.width <= x + width &&
                other.y + other.height <= y + height);
    }

    static Rect Union(const Rect& a, const Rect& b) {
        if (a.empty()) return b;
        if (b.empty()) return a;

        int minX = std::min(a.x, b.x);
        int minY = std::min(a.y, b.y);
        int maxX = std::max(a.x + a.width, b.x + b.width);
        int maxY = std::max(a.y + a.height, b.y + b.height);

        return Rect{minX, minY, maxX - minX, maxY - minY};
    }

    static Rect Intersect(const Rect& a, const Rect& b) {
        if (!a.intersects(b)) return Rect{0, 0, 0, 0};

        int minX = std::max(a.x, b.x);
        int minY = std::max(a.y, b.y);
        int maxX = std::min(a.x + a.width, b.x + b.width);
        int maxY = std::min(a.y + a.height, b.y + b.height);

        return Rect{minX, minY, maxX - minX, maxY - minY};
    }
};

} // namespace lcl::render
