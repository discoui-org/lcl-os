#pragma once

namespace lcl::ui {

struct Rect {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};

    constexpr bool isEmpty() const {
        return width <= 0.0f || height <= 0.0f;
    }

    constexpr bool containsPoint(float px, float py) const {
        return px >= x && px <= (x + width) && py >= y && py <= (y + height);
    }

    constexpr bool intersects(const Rect& other) const {
        if (isEmpty() || other.isEmpty()) {
            return false;
        }
        return !(x + width <= other.x || other.x + other.width <= x ||
                 y + height <= other.y || other.y + other.height <= y);
    }

    Rect intersection(const Rect& other) const {
        if (!intersects(other)) {
            return Rect{0.0f, 0.0f, 0.0f, 0.0f};
        }
        float rx = (x > other.x) ? x : other.x;
        float ry = (y > other.y) ? y : other.y;
        float rw = (x + width < other.x + other.width) ? (x + width - rx) : (other.x + other.width - rx);
        float rh = (y + height < other.y + other.height) ? (y + height - ry) : (other.y + other.height - ry);
        return Rect{rx, ry, rw, rh};
    }

    Rect unionWith(const Rect& other) const {
        if (isEmpty()) return other;
        if (other.isEmpty()) return *this;

        float minX = (x < other.x) ? x : other.x;
        float minY = (y < other.y) ? y : other.y;
        float maxX = (x + width > other.x + other.width) ? (x + width) : (other.x + other.width);
        float maxY = (y + height > other.y + other.height) ? (y + height) : (other.y + other.height);

        return Rect{minX, minY, maxX - minX, maxY - minY};
    }
};

} // namespace lcl::ui
