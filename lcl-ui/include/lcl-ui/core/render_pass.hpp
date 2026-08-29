#pragma once

#include "lcl-graphics/canvas.hpp"
#include <cstdint>
#include <vector>

namespace lcl::ui {

class RenderPass {
public:
    RenderPass() = default;
    ~RenderPass() = default;

    void addDirtyRect(const graphics::RectF& rect);
    void clear();

    const std::vector<graphics::RectF>& getDirtyRects() const { return m_dirtyRects; }
    graphics::RectF getDamageRect() const;

    bool hasDamage() const { return !getDamageRect().isEmpty(); }

    void begin(graphics::Canvas& canvas,
               const std::vector<graphics::RectF>& frameDamageRects = {});
    void end(graphics::Canvas& canvas);

    bool isInPass() const noexcept { return m_inPass; }
    uint64_t passSerial() const noexcept { return m_passSerial; }
    const std::vector<graphics::RectF>& frameDamageRects() const noexcept {
        return m_frameDamageRects;
    }

private:
    std::vector<graphics::RectF> m_dirtyRects;
    std::vector<graphics::RectF> m_frameDamageRects;
    uint64_t m_passSerial{0};
    bool m_inPass{false};
};

} // namespace lcl::ui
