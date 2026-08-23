#include "lcl-graphics/display_list.hpp"

namespace lcl::graphics {

const std::vector<DisplayCommand>& DisplayList::commands() const noexcept {
    static const std::vector<DisplayCommand> empty;
    return m_commands ? *m_commands : empty;
}

void DisplayListBuilder::save() { m_commands.emplace_back(SaveCommand{}); }
void DisplayListBuilder::restore() { m_commands.emplace_back(RestoreCommand{}); }
void DisplayListBuilder::concat(const Matrix3& transform) {
    m_commands.emplace_back(ConcatCommand{transform});
}
void DisplayListBuilder::beginLayer(float opacity) {
    m_commands.emplace_back(BeginLayerCommand{opacity});
}
void DisplayListBuilder::endLayer() { m_commands.emplace_back(EndLayerCommand{}); }
void DisplayListBuilder::clipRect(const RectF& rect) {
    m_commands.emplace_back(ClipRectCommand{rect});
}
void DisplayListBuilder::clipPath(const Path& path, FillRule fillRule) {
    m_commands.emplace_back(ClipPathCommand{path, fillRule});
}
void DisplayListBuilder::clearRect(const RectF& rect, Color color) {
    m_commands.emplace_back(ClearRectCommand{rect, color});
}
void DisplayListBuilder::beginCachedLayer(uint64_t id, const RectF& sourceBounds) {
    m_commands.emplace_back(BeginCachedLayerCommand{id, sourceBounds});
}
void DisplayListBuilder::endCachedLayer() {
    m_commands.emplace_back(EndCachedLayerCommand{});
}
void DisplayListBuilder::drawCachedLayer(uint64_t id, const RectF& destination,
                                         float opacity) {
    m_commands.emplace_back(DrawCachedLayerCommand{id, destination, opacity});
}
void DisplayListBuilder::drawPath(const Path& path, const Paint& paint) {
    m_commands.emplace_back(DrawPathCommand{path, paint});
}
void DisplayListBuilder::drawText(PointF origin, std::string text, Color color,
                                  float fontSize, FontFamily fontFamily,
                                  bool rasterized) {
    m_commands.emplace_back(DrawTextCommand{origin, std::move(text), color,
                                            fontSize, fontFamily, rasterized});
}
void DisplayListBuilder::drawImage(const RectF& destination, uintptr_t resourceKey,
                                   int sourceWidth, int sourceHeight, int stridePixels,
                                   float opacity,
                                   float cornerRadius, float cornerRoundness,
                                   bool squareTopCorners) {
    m_commands.emplace_back(DrawImageCommand{destination, resourceKey, sourceWidth,
                                              sourceHeight, stridePixels, opacity, cornerRadius,
                                              cornerRoundness, squareTopCorners});
}
DisplayList DisplayListBuilder::build() const {
    return DisplayList(std::make_shared<const std::vector<DisplayCommand>>(m_commands));
}
void DisplayListBuilder::reset() { m_commands.clear(); }

} // namespace lcl::graphics
