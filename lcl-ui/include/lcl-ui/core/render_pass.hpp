#pragma once

#include "lcl-ui/core/rect.hpp"
#include <vector>

class SkCanvas;

namespace lcl::ui {

class RenderPass {
public:
    RenderPass() = default;
    ~RenderPass() = default;

    void addDirtyRect(const Rect& rect);
    void clear();

    const std::vector<Rect>& getDirtyRects() const { return m_dirtyRects; }
    Rect getDamageRect() const;

    bool hasDamage() const { return !getDamageRect().isEmpty(); }

    void begin(SkCanvas* canvas);
    void end(SkCanvas* canvas);

private:
    std::vector<Rect> m_dirtyRects;
    bool m_inPass{false};
};

} // namespace lcl::ui
