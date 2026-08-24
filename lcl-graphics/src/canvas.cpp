#include "lcl-graphics/canvas.hpp"

#include <type_traits>

namespace lcl::graphics {

void Canvas::drawDisplayList(const DisplayList& displayList) {
    for (const auto& command : displayList.commands()) {
        std::visit([this](const auto& op) {
            using T = std::decay_t<decltype(op)>;
            if constexpr (std::is_same_v<T, SaveCommand>) {
                saveState();
            } else if constexpr (std::is_same_v<T, RestoreCommand>) {
                restoreState();
            } else if constexpr (std::is_same_v<T, ConcatCommand>) {
                concatTransform(op.transform);
            } else if constexpr (std::is_same_v<T, BeginLayerCommand>) {
                beginLayer(op.opacity);
            } else if constexpr (std::is_same_v<T, EndLayerCommand>) {
                endLayer();
            } else if constexpr (std::is_same_v<T, ClipRectCommand>) {
                clipRect(op.rect);
            } else if constexpr (std::is_same_v<T, ClipPathCommand>) {
                clipPath(op.path, op.fillRule);
            } else if constexpr (std::is_same_v<T, ClearRectCommand>) {
                clearRect(op.rect, op.color);
            } else if constexpr (std::is_same_v<T, BeginCachedLayerCommand>) {
                if (op.updateBounds) {
                    beginCachedLayerUpdate(op.id, op.sourceBounds,
                                           *op.updateBounds);
                } else {
                    beginCachedLayer(op.id, op.sourceBounds);
                }
            } else if constexpr (std::is_same_v<T, EndCachedLayerCommand>) {
                endCachedLayer();
            } else if constexpr (std::is_same_v<T, DrawCachedLayerCommand>) {
                drawCachedLayer(op.id, op.destination, op.opacity);
            } else if constexpr (std::is_same_v<T, DrawPathCommand>) {
                drawPath(op.path, op.paint);
            } else if constexpr (std::is_same_v<T, DrawTextCommand>) {
                if (op.rasterized) {
                    drawRasterizedText(op.origin.x, op.origin.y, op.text, op.color,
                                       op.fontSize, op.fontFamily);
                } else {
                    drawText(op.origin.x, op.origin.y, op.text, op.color,
                             op.fontSize, op.fontFamily);
                }
            } else if constexpr (std::is_same_v<T, DrawImageCommand>) {
                const auto* pixels = reinterpret_cast<const uint32_t*>(op.resourceKey);
                drawImageResource(
                    op.destination,
                    {op.resourceId, op.contentRevision,
                     op.sourceWidth, op.sourceHeight, pixels,
                     op.stridePixels > 0 ? op.stridePixels : op.sourceWidth,
                     op.opaque},
                    op.opacity, op.cornerRadius, op.cornerRoundness,
                    op.squareTopCorners);
            }
        }, command);
    }
}

} // namespace lcl::graphics
