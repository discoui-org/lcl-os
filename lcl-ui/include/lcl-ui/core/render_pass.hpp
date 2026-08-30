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
    /**
     * Schedule pixels that can be satisfied by retained composition without
     * rerasterizing widget content (for example a ScrollView translation).
     */
    void addCompositingDirtyRect(const graphics::RectF& rect);
    void clear();

    const std::vector<graphics::RectF>& getDirtyRects() const { return m_dirtyRects; }
    const std::vector<graphics::RectF>& getRasterDirtyRects() const {
        return m_rasterDirtyRects;
    }
    graphics::RectF getDamageRect() const;

    bool hasDamage() const { return !getDamageRect().isEmpty(); }

    void begin(graphics::Canvas& canvas,
               const std::vector<graphics::RectF>& frameDamageRects = {});
    void begin(graphics::Canvas& canvas,
               const std::vector<graphics::RectF>& frameDamageRects,
               const std::vector<graphics::RectF>& frameRasterDamageRects);
    void end(graphics::Canvas& canvas);

    bool isInPass() const noexcept { return m_inPass; }
    uint64_t passSerial() const noexcept { return m_passSerial; }
    const std::vector<graphics::RectF>& frameDamageRects() const noexcept {
        return m_frameDamageRects;
    }
    const std::vector<graphics::RectF>& frameRasterDamageRects() const noexcept {
        return m_frameRasterDamageRects;
    }

private:
    static void addMergedDirtyRect(std::vector<graphics::RectF>& regions,
                                   const graphics::RectF& rect);

    std::vector<graphics::RectF> m_dirtyRects;
    std::vector<graphics::RectF> m_rasterDirtyRects;
    std::vector<graphics::RectF> m_frameDamageRects;
    std::vector<graphics::RectF> m_frameRasterDamageRects;
    uint64_t m_passSerial{0};
    bool m_inPass{false};
};

} // namespace lcl::ui
