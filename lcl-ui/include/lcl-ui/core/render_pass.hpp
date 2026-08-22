#pragma once

#include "lcl-graphics/canvas.hpp"
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

    void begin(graphics::Canvas& canvas);
    void end(graphics::Canvas& canvas);

private:
    std::vector<graphics::RectF> m_dirtyRects;
    bool m_inPass{false};
};

} // namespace lcl::ui
