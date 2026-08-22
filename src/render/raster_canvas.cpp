#include "render/raster_canvas.hpp"
#include "render/path_rasterizer.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace lcl::render {

RasterCanvas::RasterCanvas()
    : m_clientEglContext(std::make_unique<ClientEGLContext>()),
      m_ownedRenderer(std::make_unique<RasterRenderer>()),
      m_renderer(m_ownedRenderer.get()) {}

RasterCanvas::RasterCanvas(RasterRenderer& renderer) : m_renderer(&renderer) {}

RasterCanvas::~RasterCanvas() {
    if (m_cachedLayerCanvasState) endCachedLayer();
    clearCachedLayers();
    if (m_renderer) m_renderer->setRetainsFrameBacking(false);
}

RasterColor RasterCanvas::toRaster(lcl::graphics::Color color) {
    return {color.r, color.g, color.b, color.a};
}

bool RasterCanvas::initialize(uint32_t width, uint32_t height, uint32_t* targetPixels) {
    if (m_renderTarget.pixelSize.width == 0 || m_renderTarget.pixelSize.height == 0) {
        m_renderTarget = {{static_cast<float>(width), static_cast<float>(height)},
                          {width, height}, 1.0f};
    }
    if (m_clientEglContext && m_clientEglContext->initialize(width, height)) {
        if (!m_clientEglContext->hasDmaBufPool()) {
            std::cerr << "[LCL Canvas] Client DMA-BUF unavailable; retaining GPU-to-SHM transport\n";
        }
        const bool initialized = renderer().initialize(
            width, height, m_clientEglContext.get(), targetPixels);
        if (initialized) renderer().setRetainsFrameBacking(true);
        return initialized;
    }
    const bool initialized = renderer().initialize(width, height, nullptr, targetPixels);
    if (initialized) renderer().setRetainsFrameBacking(true);
    return initialized;
}

void RasterCanvas::setTargetPixels(uint32_t* targetPixels, uint32_t width, uint32_t height) {
    if (width > 0 && height > 0) m_renderTarget.pixelSize = {width, height};
    renderer().setTargetPixels(targetPixels, width, height);
}

void RasterCanvas::setRenderTarget(const lcl::graphics::RenderTarget& target) {
    lcl::graphics::RenderTarget sanitized = target;
    if (!std::isfinite(sanitized.deviceScale) || sanitized.deviceScale < 0.5f ||
        sanitized.deviceScale > 4.0f) {
        sanitized.deviceScale = 1.0f;
    }
    if (std::fabs(m_renderTarget.deviceScale - sanitized.deviceScale) > 0.0001f) {
        m_textLayers.clear();
        clearCachedLayers();
    }
    m_renderTarget = sanitized;
    renderer().setDeviceScale(m_renderTarget.deviceScale);
}
void RasterCanvas::beginFrame() {
    m_dmaBufFrameActive = false;
    m_dmaBufFrameBlocked = false;
    m_state = CanvasState{};
    m_stack.clear();
    m_layerOpacityStack.clear();
    m_displayListBuilder.reset();
    syncRendererClip();
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
void RasterCanvas::endFrame() {
    m_lastDisplayList = m_displayListBuilder.build();
    if (!m_dmaBufFrameBlocked) renderer().endFrame();
}
uint32_t* RasterCanvas::rasterBuffer() { return renderer().getRasterBuffer(); }
bool RasterCanvas::isDmaBufFrameActive() const { return m_dmaBufFrameActive; }
void RasterCanvas::setDmaBufTransportEnabled(bool enabled) {
    m_dmaBufTransportEnabled = enabled;
    if (!enabled && m_clientEglContext) m_clientEglContext->cancelCurrentDmaBuf();
}
bool RasterCanvas::hasDmaBufTransport() const {
    return m_dmaBufTransportEnabled && m_clientEglContext && m_clientEglContext->hasDmaBufPool();
}
bool RasterCanvas::configureDmaBufFrame(uint32_t contentWidth, uint32_t contentHeight,
                                      uint32_t backingWidth, uint32_t backingHeight) {
    if (!m_clientEglContext || contentWidth == 0 || contentHeight == 0 ||
        backingWidth < contentWidth || backingHeight < contentHeight ||
        !m_clientEglContext->ensureDmaBufCapacity(backingWidth, backingHeight)) return false;
    m_dmaBufContentWidth = contentWidth;
    m_dmaBufContentHeight = contentHeight;
    renderer().setFrameExtent(contentWidth, contentHeight);
    return renderer().ensureFrameBackingCapacity(backingWidth, backingHeight);
}
bool RasterCanvas::isDmaBufFrameBlocked() const { return m_dmaBufFrameBlocked; }

std::optional<lcl::graphics::DmaBufFrame> RasterCanvas::takeDmaBufFrame() {
    if (!m_dmaBufFrameActive || !m_clientEglContext) return std::nullopt;
    m_dmaBufFrameActive = false;
    renderer().clearExternalFrameTarget();
    const auto exported = m_clientEglContext->exportCurrentDmaBuf();
    if (!exported) return std::nullopt;
    return lcl::graphics::DmaBufFrame{exported->bufferId, m_dmaBufContentWidth, m_dmaBufContentHeight,
                                exported->width, exported->height,
                                exported->stride, exported->format, exported->modifier,
                                exported->fd};
}

void RasterCanvas::cancelDmaBufFrame(uint32_t bufferId) {
    if (!m_clientEglContext) return;
    m_clientEglContext->cancelCurrentDmaBuf();
    m_clientEglContext->releaseDmaBuf(bufferId);
}

void RasterCanvas::releaseDmaBufFrame(uint32_t bufferId) {
    if (m_clientEglContext) m_clientEglContext->releaseDmaBuf(bufferId);
}

void RasterCanvas::syncRendererClip() {
    if (m_renderer) {
        if (m_state.clip.has_value()) {
            m_renderer->setClipRect(RasterRect{
                m_state.clip->x,
                m_state.clip->y,
                m_state.clip->width,
                m_state.clip->height
            });
        } else {
            m_renderer->setClipRect(std::nullopt);
        }
    }
}

void RasterCanvas::saveState() {
    m_displayListBuilder.save();
    m_stack.push_back(m_state);
}

void RasterCanvas::restoreState() {
    if (m_stack.empty()) return;
    m_displayListBuilder.restore();
    m_state = m_stack.back();
    m_stack.pop_back();
    syncRendererClip();
}

void RasterCanvas::clipRect(const lcl::graphics::RectF& rect) {
    m_displayListBuilder.clipRect(rect);
    lcl::graphics::RectF mapped = mapRect(rect);
    m_state.clip = m_state.clip ? m_state.clip->intersection(mapped) : mapped;
    syncRendererClip();
}

void RasterCanvas::clipPath(const lcl::graphics::Path& path,
                            lcl::graphics::FillRule fillRule) {
    m_displayListBuilder.clipPath(path, fillRule);
    const auto contours = lcl::graphics::flattenPath(path, m_state.transform);
    lcl::graphics::RectF bounds{};
    bool initialized = false;
    for (const auto& contour : contours) {
        for (const auto& point : contour.points) {
            if (!initialized) {
                bounds = {point.x, point.y, 0.0f, 0.0f};
                initialized = true;
            } else {
                const float left = std::min(bounds.x, point.x);
                const float top = std::min(bounds.y, point.y);
                const float right = std::max(bounds.x + bounds.width, point.x);
                const float bottom = std::max(bounds.y + bounds.height, point.y);
                bounds = {left, top, right - left, bottom - top};
            }
        }
    }
    if (!initialized) return;
    m_state.clip = m_state.clip ? m_state.clip->intersection(bounds) : bounds;
    syncRendererClip();
}

void RasterCanvas::concatTransform(const lcl::graphics::Matrix3& value) {
    m_displayListBuilder.concat(value);
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

void RasterCanvas::beginLayer(float opacity) {
    m_displayListBuilder.beginLayer(opacity);
    m_layerOpacityStack.push_back(m_state.opacity);
    m_state.opacity *= std::clamp(opacity, 0.0f, 1.0f);
}

void RasterCanvas::endLayer() {
    if (m_layerOpacityStack.empty()) return;
    m_displayListBuilder.endLayer();
    m_state.opacity = m_layerOpacityStack.back();
    m_layerOpacityStack.pop_back();
}

bool RasterCanvas::beginCachedLayer(CachedLayerId id,
                                  const lcl::graphics::RectF& sourceBounds) {
    if (m_cachedLayerCanvasState || sourceBounds.width <= 0.0f ||
        sourceBounds.height <= 0.0f) return false;
    const float effectiveScale = std::max(
        0.001f, m_renderTarget.deviceScale * m_state.transform.maxScale());
    const uint32_t pixelWidth = std::max(
        1u, static_cast<uint32_t>(std::ceil(sourceBounds.width * effectiveScale)));
    const uint32_t pixelHeight = std::max(
        1u, static_cast<uint32_t>(std::ceil(sourceBounds.height * effectiveScale)));

    auto& layer = m_cachedLayers[id];
    const auto sameTransform = [](const auto& lhs, const auto& rhs) {
        return std::fabs(lhs.a - rhs.a) < 0.0001f &&
               std::fabs(lhs.b - rhs.b) < 0.0001f &&
               std::fabs(lhs.c - rhs.c) < 0.0001f &&
               std::fabs(lhs.d - rhs.d) < 0.0001f &&
               std::fabs(lhs.tx - rhs.tx) < 0.0001f &&
               std::fabs(lhs.ty - rhs.ty) < 0.0001f;
    };
    if (layer.pixelWidth != pixelWidth || layer.pixelHeight != pixelHeight ||
        std::fabs(layer.effectiveScale - effectiveScale) > 0.0001f ||
        !sameTransform(layer.transform, m_state.transform)) {
        renderer().destroyCachedLayerTarget(layer.framebuffer, layer.texture);
        layer = CachedLayer{};
        layer.pixelWidth = pixelWidth;
        layer.pixelHeight = pixelHeight;
        layer.transform = m_state.transform;
        layer.effectiveScale = effectiveScale;
    }

    const bool gpu = renderer().getBackendType() == RasterBackend::OpenGL_EGL;
    if (gpu && layer.framebuffer == 0 &&
        !renderer().createCachedLayerTarget(pixelWidth, pixelHeight,
                                            layer.framebuffer, layer.texture)) {
        m_cachedLayers.erase(id);
        return false;
    }
    if (!gpu && layer.pixels.size() != static_cast<size_t>(pixelWidth) * pixelHeight) {
        layer.pixels.resize(static_cast<size_t>(pixelWidth) * pixelHeight);
    }

    uint32_t* softwarePixels = gpu ? nullptr : layer.pixels.data();
    if (!renderer().beginCachedLayerTarget(
            layer.framebuffer, layer.texture, pixelWidth, pixelHeight,
            softwarePixels, sourceBounds.x, sourceBounds.y, effectiveScale)) {
        return false;
    }

    m_cachedLayerCanvasState = m_state;
    m_state = CanvasState{};
    return true;
}

void RasterCanvas::endCachedLayer() {
    if (!m_cachedLayerCanvasState) return;
    renderer().endCachedLayerTarget();
    m_state = *m_cachedLayerCanvasState;
    m_cachedLayerCanvasState.reset();
    syncRendererClip();
}

bool RasterCanvas::drawCachedLayer(CachedLayerId id,
                                 const lcl::graphics::RectF& destination,
                                 float opacity) {
    const auto found = m_cachedLayers.find(id);
    if (found == m_cachedLayers.end()) return false;
    const auto& layer = found->second;
    const lcl::graphics::RectF mapped = mapRect(destination);
    if (m_state.clip && !m_state.clip->intersects(mapped)) return true;
    opacity = std::clamp(opacity * m_state.opacity, 0.0f, 1.0f);
    if (layer.texture != 0) {
        renderer().drawCachedLayerTexture(
            layer.texture, {mapped.x, mapped.y, mapped.width, mapped.height}, opacity);
        return true;
    }
    if (layer.pixels.empty()) return false;
    renderer().drawBufferTransformed(
        mapped.x, mapped.y, static_cast<int>(layer.pixelWidth),
        static_cast<int>(layer.pixelHeight), layer.pixels.data(),
        static_cast<int>(layer.pixelWidth), opacity, 0.0f, 2.0f, false,
        mapped.width, mapped.height);
    return true;
}

void RasterCanvas::clearCachedLayers() {
    for (const auto& [_, layer] : m_cachedLayers) {
        renderer().destroyCachedLayerTarget(layer.framebuffer, layer.texture);
    }
    m_cachedLayers.clear();
}

void RasterCanvas::clearRect(const lcl::graphics::RectF& rect, lcl::graphics::Color color) {
    const auto mapped = mapRect(rect);
    renderer().clearRect({mapped.x, mapped.y, mapped.width, mapped.height}, toRaster(color));
}

std::pair<float, float> RasterCanvas::mapPoint(float x, float y) const {
    const auto& t = m_state.transform;
    return {t.a * x + t.c * y + t.tx, t.b * x + t.d * y + t.ty};
}

lcl::graphics::RectF RasterCanvas::mapRect(const lcl::graphics::RectF& rect) const {
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

lcl::graphics::Color RasterCanvas::mapColor(lcl::graphics::Color color) const {
    color.a = static_cast<uint8_t>(std::clamp(std::lround(static_cast<float>(color.a) * m_state.opacity), 0l, 255l));
    return color;
}

void RasterCanvas::drawPath(const lcl::graphics::Path& path,
                            const lcl::graphics::Paint& paint) {
    if (path.empty() || paint.color.a == 0 || paint.opacity <= 0.0f) return;
    m_displayListBuilder.drawPath(path, paint);
    renderer().drawPath(path, paint, m_state.transform, m_state.opacity);
}

void RasterCanvas::drawRect(const lcl::graphics::RectF& rect, lcl::graphics::Color color) {
    lcl::graphics::Path path;
    path.addRect(rect);
    drawPath(path, {{color}, lcl::graphics::PaintStyle::Fill});
}

void RasterCanvas::drawRoundedRect(const lcl::graphics::RectF& rect, float radius,
                                 lcl::graphics::Color color, lcl::graphics::Color border,
                                 float borderWidth, float roundness) {
    lcl::graphics::Path path;
    path.addRRect({rect, radius, radius, roundness});
    if (color.a > 0) {
        drawPath(path, {{color}, lcl::graphics::PaintStyle::Fill});
    }
    if (borderWidth > 0.0f && border.a > 0) {
        lcl::graphics::Paint borderPaint{};
        borderPaint.color = border;
        borderPaint.style = lcl::graphics::PaintStyle::Stroke;
        borderPaint.stroke.width = borderWidth;
        borderPaint.stroke.join = lcl::graphics::StrokeJoin::Round;
        drawPath(path, borderPaint);
    }
}

void RasterCanvas::drawTopRoundedRect(const lcl::graphics::RectF& rect, float radius,
                                    lcl::graphics::Color color, float roundness) {
    lcl::graphics::Path path;
    const float r = std::clamp(radius, 0.0f, std::min(rect.width, rect.height));
    path.addTopRRect(rect, r, roundness);
    drawPath(path, {{color}, lcl::graphics::PaintStyle::Fill});
}

void RasterCanvas::drawText(float x, float y, const std::string& text,
                          lcl::graphics::Color color, float fontSize,
                          lcl::graphics::FontFamily family) {
    if (text.empty()) return;
    m_displayListBuilder.drawText({x, y}, text, color, fontSize, family);
    color = mapColor(color);
    const auto point = mapPoint(x, y);
    const float scale = std::sqrt(std::fabs(m_state.transform.a * m_state.transform.d -
                                            m_state.transform.b * m_state.transform.c));
    fontSize *= scale;
    const uint32_t argb = (static_cast<uint32_t>(color.a) << 24) |
                          (static_cast<uint32_t>(color.r) << 16) |
                          (static_cast<uint32_t>(color.g) << 8) |
                          static_cast<uint32_t>(color.b);
    if (family == lcl::graphics::FontFamily::Monospace) {
        renderer().drawMonospaceString(static_cast<int>(point.first), static_cast<int>(point.second), text, argb, fontSize);
    } else {
        renderer().drawString(static_cast<int>(point.first), static_cast<int>(point.second), text, argb, fontSize);
    }
}

void RasterCanvas::drawRasterizedText(float x, float y, const std::string& text,
                                    lcl::graphics::Color color, float fontSize,
                                    lcl::graphics::FontFamily family) {
    if (text.empty()) return;
    m_displayListBuilder.drawText({x, y}, text, color, fontSize, family);
    const uint32_t argb = (static_cast<uint32_t>(color.a) << 24) |
                          (static_cast<uint32_t>(color.r) << 16) |
                          (static_cast<uint32_t>(color.g) << 8) |
                          static_cast<uint32_t>(color.b);
    const float effectiveScale = std::max(
        0.001f, m_renderTarget.deviceScale * m_state.transform.maxScale());

    ++m_textLayerUseCounter;
    auto found = std::find_if(m_textLayers.begin(), m_textLayers.end(),
        [&](const TextLayer& layer) {
            return layer.text == text && layer.argb == argb && layer.family == family &&
                   std::fabs(layer.fontSize - fontSize) < 0.0001f &&
                   std::fabs(layer.effectiveScale - effectiveScale) < 0.0001f;
        });

    if (found == m_textLayers.end()) {
        TextLayer layer;
        layer.text = text;
        layer.argb = argb;
        layer.fontSize = fontSize;
        layer.effectiveScale = effectiveScale;
        layer.family = family;
        const float previousScale = renderer().getDeviceScale();
        renderer().setDeviceScale(effectiveScale);
        if (!renderer().rasterizeString(text, argb, fontSize,
                                        family == lcl::graphics::FontFamily::Monospace,
                                        layer.pixels, layer.width, layer.height)) {
            renderer().setDeviceScale(previousScale);
            drawText(x, y, text, color, fontSize, family);
            return;
        }
        renderer().setDeviceScale(previousScale);
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

    const float logicalWidth = static_cast<float>(found->width) / effectiveScale;
    const float logicalHeight = static_cast<float>(found->height) / effectiveScale;
    lcl::graphics::RectF mapped = mapRect({x, y, logicalWidth, logicalHeight});
    if (m_state.clip && !m_state.clip->intersects(mapped)) return;
    renderer().drawBufferTransformed(mapped.x, mapped.y,
        found->width, found->height, found->pixels.data(), found->width,
        m_state.opacity, 0.0f, 2.0f, false, mapped.width, mapped.height);
}

float RasterCanvas::measureText(const std::string& text, float fontSize,
                              lcl::graphics::FontFamily family) {
    return family == lcl::graphics::FontFamily::Monospace
        ? renderer().measureMonospaceString(text, fontSize)
        : renderer().measureString(text, fontSize);
}

void RasterCanvas::drawBuffer(const lcl::graphics::RectF& destination,
                            int srcWidth, int srcHeight,
                            const uint32_t* pixels, int stridePixels, float opacity,
                            float cornerRadius, float cornerRoundness,
                            bool squareTopCorners) {
    m_displayListBuilder.drawImage(destination, reinterpret_cast<uintptr_t>(pixels),
                                   srcWidth, srcHeight, opacity, cornerRadius,
                                   cornerRoundness, squareTopCorners);
    lcl::graphics::RectF mapped = mapRect(destination);
    if (m_state.clip && !m_state.clip->intersects(mapped)) return;
    renderer().drawBufferTransformed(
        mapped.x, mapped.y, srcWidth, srcHeight, pixels, stridePixels,
        opacity * m_state.opacity,
        cornerRadius * m_state.transform.maxScale(), cornerRoundness,
        squareTopCorners, mapped.width, mapped.height);
}

std::unique_ptr<lcl::graphics::Canvas> makeRasterCanvas() {
    return std::make_unique<RasterCanvas>();
}

} // namespace lcl::render
