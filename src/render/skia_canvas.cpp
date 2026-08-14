#include "render/skia_canvas.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace lcl::render {

SkiaCanvas::SkiaCanvas()
    : m_clientEglContext(std::make_unique<ClientEGLContext>()),
      m_ownedRenderer(std::make_unique<SkiaRenderer>()),
      m_renderer(m_ownedRenderer.get()) {}

SkiaCanvas::SkiaCanvas(SkiaRenderer& renderer) : m_renderer(&renderer) {}

SkiaColor SkiaCanvas::toSkia(lcl::ui::Color color) {
    return {color.r, color.g, color.b, color.a};
}

bool SkiaCanvas::initialize(uint32_t width, uint32_t height, uint32_t* targetPixels) {
    if (m_clientEglContext && m_clientEglContext->initialize(width, height)) {
        if (!m_clientEglContext->hasDmaBufPool()) {
            std::cerr << "[LCL Canvas] Client DMA-BUF unavailable; retaining GPU-to-SHM transport\n";
        }
        return renderer().initialize(width, height, m_clientEglContext.get(), targetPixels);
    }
    return renderer().initialize(width, height, nullptr, targetPixels);
}

void SkiaCanvas::setTargetPixels(uint32_t* targetPixels, uint32_t width, uint32_t height) {
    renderer().setTargetPixels(targetPixels, width, height);
}

void SkiaCanvas::setContentScale(float scale) {
    renderer().setContentScale(scale);
    const float sanitized = renderer().getContentScale();
    if (std::fabs(m_contentScale - sanitized) > 0.0001f) {
        m_contentScale = sanitized;
        m_textLayers.clear();
    }
}
void SkiaCanvas::beginFrame() {
    m_dmaBufFrameActive = false;
    m_dmaBufFrameBlocked = false;
    renderer().clearExternalFrameTarget();
    if (hasDmaBufTransport()) {
        if (const auto target = m_clientEglContext->acquireDmaBufTarget()) {
            renderer().setExternalFrameTarget(target->framebuffer, target->texture,
                                               target->width, target->height);
            m_dmaBufFrameActive = true;
        } else {
            // Never replace a live DMA-BUF frame with a CPU/SHM frame merely
            // because the compositor has not released a pool slot yet.
            m_dmaBufFrameBlocked = true;
            return;
        }
    }
    renderer().beginFrame();
}
void SkiaCanvas::endFrame() {
    if (!m_dmaBufFrameBlocked) renderer().endFrame();
}
uint32_t* SkiaCanvas::rasterBuffer() { return renderer().getRasterBuffer(); }
bool SkiaCanvas::isDmaBufFrameActive() const { return m_dmaBufFrameActive; }
void SkiaCanvas::setDmaBufTransportEnabled(bool enabled) {
    m_dmaBufTransportEnabled = enabled;
    if (!enabled && m_clientEglContext) m_clientEglContext->cancelCurrentDmaBuf();
}
bool SkiaCanvas::hasDmaBufTransport() const {
    return m_dmaBufTransportEnabled && m_clientEglContext && m_clientEglContext->hasDmaBufPool();
}
bool SkiaCanvas::configureDmaBufFrame(uint32_t contentWidth, uint32_t contentHeight,
                                      uint32_t backingWidth, uint32_t backingHeight) {
    if (!m_clientEglContext || contentWidth == 0 || contentHeight == 0 ||
        backingWidth < contentWidth || backingHeight < contentHeight ||
        !m_clientEglContext->ensureDmaBufCapacity(backingWidth, backingHeight)) return false;
    m_dmaBufContentWidth = contentWidth;
    m_dmaBufContentHeight = contentHeight;
    renderer().setFrameExtent(contentWidth, contentHeight);
    return true;
}
bool SkiaCanvas::isDmaBufFrameBlocked() const { return m_dmaBufFrameBlocked; }

std::optional<lcl::ui::DmaBufFrame> SkiaCanvas::takeDmaBufFrame() {
    if (!m_dmaBufFrameActive || !m_clientEglContext) return std::nullopt;
    m_dmaBufFrameActive = false;
    renderer().clearExternalFrameTarget();
    const auto exported = m_clientEglContext->exportCurrentDmaBuf();
    if (!exported) return std::nullopt;
    return lcl::ui::DmaBufFrame{exported->bufferId, m_dmaBufContentWidth, m_dmaBufContentHeight,
                                exported->width, exported->height,
                                exported->stride, exported->format, exported->modifier,
                                exported->fd};
}

void SkiaCanvas::cancelDmaBufFrame(uint32_t bufferId) {
    if (!m_clientEglContext) return;
    m_clientEglContext->cancelCurrentDmaBuf();
    m_clientEglContext->releaseDmaBuf(bufferId);
}

void SkiaCanvas::releaseDmaBufFrame(uint32_t bufferId) {
    if (m_clientEglContext) m_clientEglContext->releaseDmaBuf(bufferId);
}

void SkiaCanvas::saveState() { m_stack.push_back(m_state); }

void SkiaCanvas::restoreState() {
    if (m_stack.empty()) return;
    m_state = m_stack.back();
    m_stack.pop_back();
}

void SkiaCanvas::clipRect(const lcl::ui::Rect& rect) {
    lcl::ui::Rect mapped = mapRect(rect);
    m_state.clip = m_state.clip ? m_state.clip->intersection(mapped) : mapped;
}

void SkiaCanvas::concatTransform(const lcl::ui::AffineTransform& value) {
    const auto old = m_state.transform;
    m_state.transform = {
        old.a * value.a + old.c * value.b,
        old.b * value.a + old.d * value.b,
        old.a * value.c + old.c * value.d,
        old.b * value.c + old.d * value.d,
        old.a * value.tx + old.c * value.ty + old.tx,
        old.b * value.tx + old.d * value.ty + old.ty,
    };
}

void SkiaCanvas::beginLayer(float opacity) {
    m_layerOpacityStack.push_back(m_state.opacity);
    m_state.opacity *= std::clamp(opacity, 0.0f, 1.0f);
}

void SkiaCanvas::endLayer() {
    if (m_layerOpacityStack.empty()) return;
    m_state.opacity = m_layerOpacityStack.back();
    m_layerOpacityStack.pop_back();
}

std::pair<float, float> SkiaCanvas::mapPoint(float x, float y) const {
    const auto& t = m_state.transform;
    return {t.a * x + t.c * y + t.tx, t.b * x + t.d * y + t.ty};
}

lcl::ui::Rect SkiaCanvas::mapRect(const lcl::ui::Rect& rect) const {
    const auto p0 = mapPoint(rect.x, rect.y);
    const auto p1 = mapPoint(rect.x + rect.width, rect.y);
    const auto p2 = mapPoint(rect.x, rect.y + rect.height);
    const auto p3 = mapPoint(rect.x + rect.width, rect.y + rect.height);
    const float left = std::min({p0.first, p1.first, p2.first, p3.first});
    const float right = std::max({p0.first, p1.first, p2.first, p3.first});
    const float top = std::min({p0.second, p1.second, p2.second, p3.second});
    const float bottom = std::max({p0.second, p1.second, p2.second, p3.second});
    return {left, top, right - left, bottom - top};
}

lcl::ui::Color SkiaCanvas::mapColor(lcl::ui::Color color) const {
    color.a = static_cast<uint8_t>(std::clamp(std::lround(static_cast<float>(color.a) * m_state.opacity), 0l, 255l));
    return color;
}

bool SkiaCanvas::applyClip(lcl::ui::Rect& rect) const {
    if (!m_state.clip) return !rect.isEmpty();
    rect = rect.intersection(*m_state.clip);
    return !rect.isEmpty();
}

void SkiaCanvas::drawRect(const lcl::ui::Rect& rect, lcl::ui::Color color) {
    lcl::ui::Rect mapped = mapRect(rect);
    if (!applyClip(mapped)) return;
    renderer().drawRect({mapped.x, mapped.y, mapped.width, mapped.height}, toSkia(mapColor(color)));
}

void SkiaCanvas::drawRoundedRect(const lcl::ui::Rect& rect, float radius,
                                 lcl::ui::Color color, lcl::ui::Color border,
                                 float borderWidth, float roundness) {
    lcl::ui::Rect mapped = mapRect(rect);
    if (!applyClip(mapped)) return;
    const float scale = std::sqrt(std::fabs(m_state.transform.a * m_state.transform.d -
                                            m_state.transform.b * m_state.transform.c));
    renderer().drawRoundedRect({mapped.x, mapped.y, mapped.width, mapped.height}, radius * scale,
                               toSkia(mapColor(color)), toSkia(mapColor(border)), borderWidth * scale, roundness);
}

void SkiaCanvas::drawTopRoundedRect(const lcl::ui::Rect& rect, float radius,
                                    lcl::ui::Color color, float roundness) {
    lcl::ui::Rect mapped = mapRect(rect);
    if (!applyClip(mapped)) return;
    renderer().drawTopRoundedRect({mapped.x, mapped.y, mapped.width, mapped.height}, radius,
                                  toSkia(mapColor(color)), roundness);
}

void SkiaCanvas::drawText(float x, float y, const std::string& text,
                          lcl::ui::Color color, float fontSize,
                          lcl::ui::FontFamily family) {
    color = mapColor(color);
    const auto point = mapPoint(x, y);
    const float scale = std::sqrt(std::fabs(m_state.transform.a * m_state.transform.d -
                                            m_state.transform.b * m_state.transform.c));
    fontSize *= scale;
    const uint32_t argb = (static_cast<uint32_t>(color.a) << 24) |
                          (static_cast<uint32_t>(color.r) << 16) |
                          (static_cast<uint32_t>(color.g) << 8) |
                          static_cast<uint32_t>(color.b);
    if (family == lcl::ui::FontFamily::Monospace) {
        renderer().drawMonospaceString(static_cast<int>(point.first), static_cast<int>(point.second), text, argb, fontSize);
    } else {
        renderer().drawString(static_cast<int>(point.first), static_cast<int>(point.second), text, argb, fontSize);
    }
}

void SkiaCanvas::drawRasterizedText(float x, float y, const std::string& text,
                                    lcl::ui::Color color, float fontSize,
                                    lcl::ui::FontFamily family) {
    if (text.empty()) return;
    const uint32_t argb = (static_cast<uint32_t>(color.a) << 24) |
                          (static_cast<uint32_t>(color.r) << 16) |
                          (static_cast<uint32_t>(color.g) << 8) |
                          static_cast<uint32_t>(color.b);

    ++m_textLayerUseCounter;
    auto found = std::find_if(m_textLayers.begin(), m_textLayers.end(),
        [&](const TextLayer& layer) {
            return layer.text == text && layer.argb == argb && layer.family == family &&
                   std::fabs(layer.fontSize - fontSize) < 0.0001f &&
                   std::fabs(layer.contentScale - m_contentScale) < 0.0001f;
        });

    if (found == m_textLayers.end()) {
        TextLayer layer;
        layer.text = text;
        layer.argb = argb;
        layer.fontSize = fontSize;
        layer.contentScale = m_contentScale;
        layer.family = family;
        if (!renderer().rasterizeString(text, argb, fontSize,
                                        family == lcl::ui::FontFamily::Monospace,
                                        layer.pixels, layer.width, layer.height)) {
            drawText(x, y, text, color, fontSize, family);
            return;
        }
        layer.lastUse = m_textLayerUseCounter;
        if (m_textLayers.size() >= 96u) {
            const auto oldest = std::min_element(m_textLayers.begin(), m_textLayers.end(),
                [](const TextLayer& a, const TextLayer& b) { return a.lastUse < b.lastUse; });
            m_textLayers.erase(oldest);
        }
        m_textLayers.push_back(std::move(layer));
        found = std::prev(m_textLayers.end());
    }
    found->lastUse = m_textLayerUseCounter;

    const float logicalWidth = static_cast<float>(found->width) / m_contentScale;
    const float logicalHeight = static_cast<float>(found->height) / m_contentScale;
    lcl::ui::Rect mapped = mapRect({x, y, logicalWidth, logicalHeight});
    if (!applyClip(mapped)) return;
    renderer().drawBufferTransformed(mapped.x, mapped.y,
        found->width, found->height, found->pixels.data(), found->width,
        m_state.opacity, 0.0f, 2.0f, false, mapped.width, mapped.height);
}

float SkiaCanvas::measureText(const std::string& text, float fontSize,
                              lcl::ui::FontFamily family) {
    return family == lcl::ui::FontFamily::Monospace
        ? renderer().measureMonospaceString(text, fontSize)
        : renderer().measureString(text, fontSize);
}

void SkiaCanvas::drawBuffer(int dstX, int dstY, int srcWidth, int srcHeight,
                            const uint32_t* pixels, int stridePixels, float opacity,
                            float cornerRadius, float cornerRoundness,
                            bool squareTopCorners, int drawWidth, int drawHeight) {
    lcl::ui::Rect mapped = mapRect({static_cast<float>(dstX), static_cast<float>(dstY),
                                    static_cast<float>(drawWidth), static_cast<float>(drawHeight)});
    if (!applyClip(mapped)) return;
    renderer().drawBuffer(static_cast<int>(std::lround(mapped.x)), static_cast<int>(std::lround(mapped.y)),
                          srcWidth, srcHeight, pixels, stridePixels, opacity * m_state.opacity,
                          cornerRadius, cornerRoundness, squareTopCorners,
                          static_cast<int>(std::lround(mapped.width)), static_cast<int>(std::lround(mapped.height)));
}

std::unique_ptr<lcl::ui::Canvas> makeSkiaCanvas() {
    return std::make_unique<SkiaCanvas>();
}

} // namespace lcl::render
