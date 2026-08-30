#include "system/render/raster_canvas.hpp"
#include "system/render/text_metrics.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace lcl::render {

RasterCanvas::RasterCanvas(bool displayListOnly)
    : m_displayListOnly(displayListOnly) {
    if (!m_displayListOnly) {
        m_clientEglContext = std::make_unique<ClientEGLContext>();
        m_ownedRenderer = std::make_unique<RasterRenderer>();
        m_renderer = m_ownedRenderer.get();
    }
}

RasterCanvas::RasterCanvas(RasterRenderer& renderer) : m_renderer(&renderer) {}

RasterCanvas::~RasterCanvas() {
    if (m_cachedLayerCanvasState) endCachedLayer();
    clearCachedLayers();
    if (m_renderer) m_renderer->setRetainsFrameBacking(false);
}

bool RasterCanvas::initialize(uint32_t width, uint32_t height, uint32_t* targetPixels) {
    if (m_renderTarget.pixelSize.width == 0 || m_renderTarget.pixelSize.height == 0) {
        m_renderTarget = {{static_cast<float>(width), static_cast<float>(height)},
                          {width, height}, 1.0f};
    }
    if (m_displayListOnly) {
        return true;
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
    if (m_displayListOnly) return;
    renderer().setTargetPixels(targetPixels, width, height);
}

void RasterCanvas::setRenderTarget(const lcl::graphics::RenderTarget& target) {
    lcl::graphics::RenderTarget sanitized = target;
    if (!std::isfinite(sanitized.deviceScale) || sanitized.deviceScale < 0.5f ||
        sanitized.deviceScale > 4.0f) {
        sanitized.deviceScale = 1.0f;
    }
    if (std::fabs(m_renderTarget.deviceScale - sanitized.deviceScale) > 0.0001f) {
        clearCachedLayers();
    }
    m_renderTarget = sanitized;
    if (!m_displayListOnly) renderer().setDeviceScale(m_renderTarget.deviceScale);
}
void RasterCanvas::beginFrame() {
    m_dmaBufFrameActive = false;
    m_dmaBufFrameBlocked = false;
    m_state = CanvasState{};
    m_stack.clear();
    m_layerOpacityStack.clear();
    m_displayListBuilder.reset();
    m_frameImageResources.clear();
    if (m_displayListOnly) return;
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
    if (m_displayListOnly) return;
    if (!m_dmaBufFrameBlocked) {
        renderer().replayDisplayList(m_lastDisplayList, m_renderTarget);
        renderer().endFrame();
    }
}
uint32_t* RasterCanvas::rasterBuffer() {
    return m_displayListOnly ? nullptr : renderer().getRasterBuffer();
}
std::optional<lcl::graphics::DisplayListFrame> RasterCanvas::takeDisplayListFrame() {
    if (!m_displayListOnly) return std::nullopt;
    return lcl::graphics::DisplayListFrame{m_lastDisplayList,
                                           m_frameImageResources};
}
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
                                exported->fd,
                                exported->androidHardwareBuffer
                                    ? lcl::graphics::NativeBufferTransport::AndroidHardwareBufferV1
                                    : lcl::graphics::NativeBufferTransport::DmaBuf,
                                exported->acquireFenceFd};
}

bool RasterCanvas::supportsNativeBufferTransport(
    lcl::graphics::NativeBufferTransport transport) const {
    return transport ==
               lcl::graphics::NativeBufferTransport::AndroidHardwareBufferV1 &&
           m_clientEglContext &&
           m_clientEglContext->usesAndroidHardwareBuffer();
}

bool RasterCanvas::sendNativeBufferHandle(int socketFd, uint32_t bufferId) {
    return m_clientEglContext &&
           m_clientEglContext->sendNativeBufferHandle(socketFd, bufferId);
}

void RasterCanvas::cancelDmaBufFrame(uint32_t bufferId) {
    if (!m_clientEglContext) return;
    m_clientEglContext->cancelCurrentDmaBuf();
    m_clientEglContext->releaseDmaBuf(bufferId);
}

void RasterCanvas::releaseDmaBufFrame(uint32_t bufferId) {
    if (m_clientEglContext) m_clientEglContext->releaseDmaBuf(bufferId);
}

bool RasterCanvas::releaseDmaBufFrameWithFence(uint32_t bufferId,
                                                int releaseFenceFd) {
    if (!m_clientEglContext) return false;
    m_clientEglContext->releaseDmaBuf(bufferId, releaseFenceFd);
    return true;
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
}

void RasterCanvas::clipRect(const lcl::graphics::RectF& rect) {
    m_displayListBuilder.clipRect(rect);
    lcl::graphics::RectF mapped = m_state.transform.mapRect(rect);
    m_state.clip = m_state.clip ? m_state.clip->intersection(mapped) : mapped;
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
    return beginCachedLayerInternal(id, sourceBounds, std::nullopt);
}

bool RasterCanvas::beginCachedLayerUpdate(
        CachedLayerId id, const lcl::graphics::RectF& sourceBounds,
        const lcl::graphics::RectF& updateBounds) {
    if (updateBounds.isEmpty()) return false;
    return beginCachedLayerInternal(id, sourceBounds, updateBounds);
}

bool RasterCanvas::beginCachedLayerInternal(
        CachedLayerId id, const lcl::graphics::RectF& sourceBounds,
        std::optional<lcl::graphics::RectF> updateBounds) {
    if (m_cachedLayerCanvasState || sourceBounds.width <= 0.0f ||
        sourceBounds.height <= 0.0f) return false;
    if (m_displayListOnly) {
        if (updateBounds) {
            m_displayListBuilder.beginCachedLayerUpdate(
                id, sourceBounds, *updateBounds);
        } else {
            m_displayListBuilder.beginCachedLayer(id, sourceBounds);
        }
        m_cachedLayerCanvasState = m_state;
        m_state = CanvasState{};
        if (updateBounds) m_state.clip = *updateBounds;
        return true;
    }
    if (!renderer().prepareCachedDisplayLayer(
            id, sourceBounds, m_state.transform, m_renderTarget,
            updateBounds.has_value())) {
        return false;
    }

    if (updateBounds) {
        m_displayListBuilder.beginCachedLayerUpdate(
            id, sourceBounds, *updateBounds);
    } else {
        m_displayListBuilder.beginCachedLayer(id, sourceBounds);
    }
    m_cachedLayerCanvasState = m_state;
    m_state = CanvasState{};
    if (updateBounds) m_state.clip = *updateBounds;
    return true;
}

void RasterCanvas::endCachedLayer() {
    if (!m_cachedLayerCanvasState) return;
    m_displayListBuilder.endCachedLayer();
    m_state = *m_cachedLayerCanvasState;
    m_cachedLayerCanvasState.reset();
}

bool RasterCanvas::drawCachedLayer(CachedLayerId id,
                                 const lcl::graphics::RectF& destination,
                                 float opacity) {
    if (!m_displayListOnly && !renderer().hasCachedDisplayLayer(id)) return false;
    m_displayListBuilder.drawCachedLayer(id, destination, opacity);
    return true;
}

bool RasterCanvas::drawCachedLayerTransformed(
        CachedLayerId id, const lcl::graphics::RectF& destination,
        const lcl::graphics::Matrix3& transform, float opacity) {
    if (!m_displayListOnly && !renderer().hasCachedDisplayLayer(id)) {
        return false;
    }
    m_displayListBuilder.drawCachedLayerTransformed(
        id, destination, opacity, transform);
    return true;
}

void RasterCanvas::clearCachedLayers() {
    if (m_displayListOnly) return;
    renderer().clearDisplayListCaches();
}

void RasterCanvas::clearRect(const lcl::graphics::RectF& rect, lcl::graphics::Color color) {
    m_displayListBuilder.clearRect(rect, color);
}

void RasterCanvas::drawPath(const lcl::graphics::Path& path,
                            const lcl::graphics::Paint& paint) {
    if (path.empty() || paint.color.a == 0 || paint.opacity <= 0.0f) return;
    m_displayListBuilder.drawPath(path, paint);
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
}

float RasterCanvas::measureText(const std::string& text, float fontSize,
                              lcl::graphics::FontFamily family) {
    return lcl::render::text_metrics::measureText(text, fontSize, family);
}

void RasterCanvas::drawBuffer(const lcl::graphics::RectF& destination,
                            int srcWidth, int srcHeight,
                            const uint32_t* pixels, int stridePixels, float opacity,
                            float cornerRadius, float cornerRoundness,
                            bool squareTopCorners) {
    m_displayListBuilder.drawImage(destination, reinterpret_cast<uintptr_t>(pixels),
                                   srcWidth, srcHeight, stridePixels, opacity, cornerRadius,
                                   cornerRoundness, squareTopCorners);
}

void RasterCanvas::drawImageResource(
        const lcl::graphics::RectF& destination,
        const lcl::graphics::ImageResourceView& resource,
        float opacity, float cornerRadius, float cornerRoundness,
        bool squareTopCorners) {
    m_displayListBuilder.drawImage(
        destination, reinterpret_cast<uintptr_t>(resource.pixels),
        resource.width, resource.height, resource.stridePixels,
        opacity, cornerRadius, cornerRoundness, squareTopCorners,
        resource.id, resource.contentRevision, resource.opaque);
    if (m_displayListOnly && resource.id != 0 &&
        resource.contentRevision != 0 && resource.pixels &&
        resource.width > 0 && resource.height > 0 &&
        resource.stridePixels >= resource.width) {
        const auto duplicate = std::find_if(
            m_frameImageResources.begin(), m_frameImageResources.end(),
            [&](const auto& existing) {
                return existing.id == resource.id &&
                    existing.contentRevision == resource.contentRevision;
            });
        if (duplicate == m_frameImageResources.end()) {
            m_frameImageResources.push_back(resource);
        }
    }
}

void RasterCanvas::drawExternalBufferPlaceholder(
        uint64_t nodeId,
        const lcl::graphics::RectF& destination) {
    if (nodeId == 0 || destination.isEmpty()) return;
    m_displayListBuilder.drawExternalBuffer(nodeId, destination);
}

std::unique_ptr<lcl::graphics::Canvas> makeRasterCanvas() {
    return std::make_unique<RasterCanvas>();
}

std::unique_ptr<lcl::graphics::Canvas> makeDisplayListCanvas() {
    return std::make_unique<RasterCanvas>(true);
}

} // namespace lcl::render
