#include "render/skia_display_list_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <GLES3/gl3.h>

#include "include/core/SkBlendMode.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/SkImageGanesh.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"

#include "lcl-graphics/path.hpp"
#include "render/skia_text_engine.hpp"

namespace lcl::render {
namespace {

SkMatrix toSkMatrix(const lcl::graphics::Matrix3& value) {
    SkMatrix matrix;
    matrix.setAll(value.a, value.c, value.tx,
                  value.b, value.d, value.ty,
                  0.0f, 0.0f, 1.0f);
    return matrix;
}

SkColor4f toSkColor(const lcl::graphics::Color& color, float opacity = 1.0f) {
    constexpr float inverseByte = 1.0f / 255.0f;
    return {
        static_cast<float>(color.r) * inverseByte,
        static_cast<float>(color.g) * inverseByte,
        static_cast<float>(color.b) * inverseByte,
        static_cast<float>(color.a) * inverseByte * std::clamp(opacity, 0.0f, 1.0f),
    };
}

SkRect toSkRect(const lcl::graphics::RectF& rect) {
    return SkRect::MakeXYWH(rect.x, rect.y, rect.width, rect.height);
}

SkPath toSkPath(const lcl::graphics::Path& path,
                lcl::graphics::FillRule fillRule) {
    const auto skFill = fillRule == lcl::graphics::FillRule::EvenOdd
        ? SkPathFillType::kEvenOdd
        : SkPathFillType::kWinding;
    const bool containsArc = std::any_of(
        path.elements().begin(), path.elements().end(), [](const auto& element) {
            return element.verb == lcl::graphics::PathVerb::Arc;
        });

    SkPathBuilder builder(skFill);
    if (containsArc) {
        for (const auto& contour : lcl::graphics::flattenPath(path, {}, 0.20f)) {
            if (contour.points.empty()) continue;
            builder.moveTo(contour.points.front().x, contour.points.front().y);
            for (size_t index = 1; index < contour.points.size(); ++index) {
                builder.lineTo(contour.points[index].x, contour.points[index].y);
            }
            if (contour.closed) builder.close();
        }
        return builder.detach();
    }

    for (const auto& element : path.elements()) {
        switch (element.verb) {
            case lcl::graphics::PathVerb::MoveTo:
                builder.moveTo(element.p0.x, element.p0.y);
                break;
            case lcl::graphics::PathVerb::LineTo:
                builder.lineTo(element.p0.x, element.p0.y);
                break;
            case lcl::graphics::PathVerb::QuadTo:
                builder.quadTo(element.p0.x, element.p0.y,
                               element.p1.x, element.p1.y);
                break;
            case lcl::graphics::PathVerb::CubicTo:
                builder.cubicTo(element.p0.x, element.p0.y,
                                element.p1.x, element.p1.y,
                                element.p2.x, element.p2.y);
                break;
            case lcl::graphics::PathVerb::Arc:
                break;
            case lcl::graphics::PathVerb::Close:
                builder.close();
                break;
        }
    }
    return builder.detach();
}

SkPaint toSkPaint(const lcl::graphics::Paint& source) {
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor4f(toSkColor(source.color, source.opacity));
    if (source.style == lcl::graphics::PaintStyle::Fill) {
        paint.setStyle(SkPaint::kFill_Style);
        return paint;
    }

    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(source.stroke.scaling == lcl::graphics::StrokeScaling::Hairline
        ? 0.0f
        : std::max(0.0f, source.stroke.width));
    paint.setStrokeMiter(std::max(0.0f, source.stroke.miterLimit));
    switch (source.stroke.cap) {
        case lcl::graphics::StrokeCap::Butt:
            paint.setStrokeCap(SkPaint::kButt_Cap);
            break;
        case lcl::graphics::StrokeCap::Round:
            paint.setStrokeCap(SkPaint::kRound_Cap);
            break;
        case lcl::graphics::StrokeCap::Square:
            paint.setStrokeCap(SkPaint::kSquare_Cap);
            break;
    }
    switch (source.stroke.join) {
        case lcl::graphics::StrokeJoin::Miter:
            paint.setStrokeJoin(SkPaint::kMiter_Join);
            break;
        case lcl::graphics::StrokeJoin::Round:
            paint.setStrokeJoin(SkPaint::kRound_Join);
            break;
        case lcl::graphics::StrokeJoin::Bevel:
            paint.setStrokeJoin(SkPaint::kBevel_Join);
            break;
    }
    return paint;
}

} // namespace

struct SkiaDisplayListRenderer::Impl {
    struct ImageResource {
        uint64_t revision{0};
        sk_sp<SkImage> image;
    };

    struct CachedLayer {
        lcl::graphics::RectF sourceBounds{};
        float effectiveScale{1.0f};
        int pixelWidth{0};
        int pixelHeight{0};
        sk_sp<SkSurface> surface;
    };

    lcl::platform::IGraphicsContext* graphicsContext{nullptr};
    sk_sp<GrDirectContext> directContext;
    sk_sp<SkSurface> surface;
    std::unordered_map<uint64_t, ImageResource> imageResources;
    std::unordered_map<uint64_t, CachedLayer> cachedLayers;
    uint32_t surfaceFramebuffer{0};
    uint32_t surfaceWidth{0};
    uint32_t surfaceHeight{0};
    uint32_t* surfacePixels{nullptr};
    int frameSaveCount{0};

    sk_sp<SkImage> image(const lcl::graphics::DrawImageCommand& command) {
        const auto* pixels = reinterpret_cast<const uint32_t*>(command.resourceKey);
        if (!pixels || command.sourceWidth <= 0 || command.sourceHeight <= 0 ||
            command.stridePixels < command.sourceWidth) {
            return nullptr;
        }
        if (command.resourceId != 0 && command.contentRevision != 0) {
            const auto found = imageResources.find(command.resourceId);
            if (found != imageResources.end() &&
                found->second.revision == command.contentRevision) {
                return found->second.image;
            }
        }

        const SkImageInfo info = SkImageInfo::Make(
            command.sourceWidth, command.sourceHeight,
            kBGRA_8888_SkColorType,
            command.opaque ? kOpaque_SkAlphaType : kUnpremul_SkAlphaType,
            SkColorSpace::MakeSRGB());
        const SkPixmap pixmap(
            info, pixels,
            static_cast<size_t>(command.stridePixels) * sizeof(uint32_t));
        auto result = SkImages::RasterFromPixmapCopy(pixmap);
        if (result && command.resourceId != 0 && command.contentRevision != 0) {
            if (imageResources.size() >= 128u &&
                !imageResources.contains(command.resourceId)) {
                imageResources.erase(imageResources.begin());
            }
            imageResources[command.resourceId] = {
                command.contentRevision, result,
            };
        }
        return result;
    }

    sk_sp<SkImage> externalImage(
            const lcl::graphics::DrawExternalBufferCommand& command) {
        if (command.sourceWidth <= 0 || command.sourceHeight <= 0 ||
            command.resourceKey == 0) {
            return nullptr;
        }
        if (command.sampleKind ==
                lcl::graphics::ExternalBufferSampleKind::ArgbPixels) {
            if (command.stridePixels < command.sourceWidth) return nullptr;
            const SkImageInfo info = SkImageInfo::Make(
                command.sourceWidth, command.sourceHeight,
                kBGRA_8888_SkColorType, kPremul_SkAlphaType,
                SkColorSpace::MakeSRGB());
            const SkPixmap pixmap(
                info, reinterpret_cast<const uint32_t*>(command.resourceKey),
                static_cast<size_t>(command.stridePixels) * sizeof(uint32_t));
            return SkImages::RasterFromPixmapCopy(pixmap);
        }
        if (command.sampleKind !=
                lcl::graphics::ExternalBufferSampleKind::GlTexture ||
            !directContext) {
            return nullptr;
        }
        const GrGLTextureInfo textureInfo{
            GL_TEXTURE_2D,
            static_cast<GrGLuint>(command.resourceKey),
            GL_RGBA8,
        };
        const auto backendTexture = GrBackendTextures::MakeGL(
            command.sourceWidth, command.sourceHeight,
            skgpu::Mipmapped::kNo, textureInfo);
        if (!backendTexture.isValid()) return nullptr;
        return SkImages::BorrowTextureFrom(
            directContext.get(), backendTexture,
            kBottomLeft_GrSurfaceOrigin, kRGBA_8888_SkColorType,
            kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    }
};

SkiaDisplayListRenderer::SkiaDisplayListRenderer()
    : m_impl(std::make_unique<Impl>()) {}

SkiaDisplayListRenderer::~SkiaDisplayListRenderer() {
    shutdown();
}

bool SkiaDisplayListRenderer::initialize(
        lcl::platform::IGraphicsContext* graphicsContext) {
    shutdown();
    if (graphicsContext) {
        // Ganesh requires a valid GL context, not a physical GPU. Mesa's
        // llvmpipe is software-emulated but still exposes a complete EGL/GLES
        // context and must use the same DisplayList replay path.
        if (!graphicsContext->makeCurrent()) {
            std::cerr << "[LCL Skia] Failed to make the EGL context current.\n";
            return false;
        }
        auto interface = GrGLMakeNativeInterface();
        if (!interface || !interface->validate()) {
            std::cerr << "[LCL Skia] Failed to create a valid native GLES interface.\n";
            return false;
        }
        m_impl->directContext = GrDirectContexts::MakeGL(std::move(interface));
        if (!m_impl->directContext) {
            std::cerr << "[LCL Skia] Failed to create the Ganesh GL context.\n";
            return false;
        }
    }
    m_impl->graphicsContext = graphicsContext;
    return true;
}

void SkiaDisplayListRenderer::shutdown() {
    if (!m_impl) return;
    m_impl->surface.reset();
    m_impl->imageResources.clear();
    m_impl->cachedLayers.clear();
    if (m_impl->directContext) {
        m_impl->directContext->abandonContext();
        m_impl->directContext.reset();
    }
    m_impl->graphicsContext = nullptr;
    m_impl->surfaceFramebuffer = 0;
    m_impl->surfaceWidth = 0;
    m_impl->surfaceHeight = 0;
    m_impl->surfacePixels = nullptr;
    m_impl->frameSaveCount = 0;
}

bool SkiaDisplayListRenderer::beginFrame(
        uint32_t framebuffer, uint32_t pixelWidth, uint32_t pixelHeight,
        uint32_t* rasterPixels,
        std::optional<lcl::graphics::RectF> deviceDamage) {
    if (pixelWidth == 0 || pixelHeight == 0) {
        return false;
    }

    if (m_impl->graphicsContext) {
        if (!m_impl->directContext ||
            !m_impl->graphicsContext->makeCurrent()) return false;
        m_impl->directContext->resetContext();
        if (!m_impl->surface || m_impl->surfaceFramebuffer != framebuffer ||
            m_impl->surfaceWidth != pixelWidth ||
            m_impl->surfaceHeight != pixelHeight) {
            GLint samples = 0;
            GLint stencilBits = 0;
            glGetIntegerv(GL_SAMPLES, &samples);
            glGetIntegerv(GL_STENCIL_BITS, &stencilBits);
            const GrGLFramebufferInfo info{
                static_cast<GrGLuint>(framebuffer),
                static_cast<GrGLenum>(GL_RGBA8),
            };
            const auto target = GrBackendRenderTargets::MakeGL(
                static_cast<int>(pixelWidth), static_cast<int>(pixelHeight),
                std::max(0, samples), std::max(0, stencilBits), info);
            m_impl->surface = SkSurfaces::WrapBackendRenderTarget(
                m_impl->directContext.get(), target,
                kBottomLeft_GrSurfaceOrigin, kRGBA_8888_SkColorType,
                SkColorSpace::MakeSRGB(), nullptr);
        }
    } else {
        if (!rasterPixels) return false;
        if (!m_impl->surface || m_impl->surfacePixels != rasterPixels ||
            m_impl->surfaceWidth != pixelWidth ||
            m_impl->surfaceHeight != pixelHeight) {
            const SkImageInfo info = SkImageInfo::Make(
                static_cast<int>(pixelWidth), static_cast<int>(pixelHeight),
                kBGRA_8888_SkColorType, kUnpremul_SkAlphaType,
                SkColorSpace::MakeSRGB());
            m_impl->surface = SkSurfaces::WrapPixels(
                info, rasterPixels,
                static_cast<size_t>(pixelWidth) * sizeof(uint32_t));
        }
    }
    if (!m_impl->surface) return false;
    m_impl->surfaceFramebuffer = framebuffer;
    m_impl->surfaceWidth = pixelWidth;
    m_impl->surfaceHeight = pixelHeight;
    m_impl->surfacePixels = rasterPixels;

    SkCanvas* canvas = m_impl->surface->getCanvas();
    canvas->resetMatrix();
    m_impl->frameSaveCount = canvas->getSaveCount();
    canvas->save();
    if (deviceDamage && !deviceDamage->isEmpty()) {
        canvas->clipRect(toSkRect(*deviceDamage), SkClipOp::kIntersect, false);
    }
    return true;
}

bool SkiaDisplayListRenderer::replay(
        const lcl::graphics::DisplayList& displayList,
        const lcl::graphics::RenderTarget& target,
        const lcl::graphics::Matrix3& rootTransform,
        float deviceOriginX, float deviceOriginY) {
    if (!m_impl->surface) return false;
    SkCanvas* canvas = m_impl->surface->getCanvas();
    const int replaySaveCount = canvas->getSaveCount();
    canvas->save();
    canvas->translate(deviceOriginX, deviceOriginY);
    canvas->scale(target.deviceScale, target.deviceScale);
    canvas->concat(toSkMatrix(rootTransform));

    struct CachedLayerReplayState {
        SkCanvas* outerCanvas{nullptr};
        int cachedCanvasSaveCount{0};
        int outerLogicalSaveDepth{0};
        int outerLogicalLayerDepth{0};
    };

    int logicalSaveDepth = 0;
    int logicalLayerDepth = 0;
    bool replaySucceeded = true;
    std::optional<CachedLayerReplayState> cachedLayerReplayState;
    for (const auto& command : displayList.commands()) {
        std::visit([&](const auto& op) {
            if (!replaySucceeded) return;
            using T = std::decay_t<decltype(op)>;
            if constexpr (std::is_same_v<T, lcl::graphics::SaveCommand>) {
                canvas->save();
                ++logicalSaveDepth;
            } else if constexpr (std::is_same_v<T, lcl::graphics::RestoreCommand>) {
                if (logicalSaveDepth > 0) {
                    canvas->restore();
                    --logicalSaveDepth;
                }
            } else if constexpr (std::is_same_v<T, lcl::graphics::ConcatCommand>) {
                canvas->concat(toSkMatrix(op.transform));
            } else if constexpr (std::is_same_v<T, lcl::graphics::BeginLayerCommand>) {
                canvas->saveLayerAlphaf(nullptr, std::clamp(op.opacity, 0.0f, 1.0f));
                ++logicalLayerDepth;
            } else if constexpr (std::is_same_v<T, lcl::graphics::EndLayerCommand>) {
                if (logicalLayerDepth > 0) {
                    canvas->restore();
                    --logicalLayerDepth;
                }
            } else if constexpr (std::is_same_v<T, lcl::graphics::ClipRectCommand>) {
                canvas->clipRect(toSkRect(op.rect), SkClipOp::kIntersect, true);
            } else if constexpr (std::is_same_v<T, lcl::graphics::ClipPathCommand>) {
                canvas->clipPath(toSkPath(op.path, op.fillRule),
                                 SkClipOp::kIntersect, true);
            } else if constexpr (std::is_same_v<T, lcl::graphics::ClearRectCommand>) {
                SkPaint paint;
                paint.setBlendMode(SkBlendMode::kSrc);
                paint.setColor4f(toSkColor(op.color));
                canvas->drawRect(toSkRect(op.rect), paint);
            } else if constexpr (std::is_same_v<T, lcl::graphics::BeginCachedLayerCommand>) {
                if (cachedLayerReplayState || op.sourceBounds.isEmpty()) {
                    replaySucceeded = false;
                    return;
                }

                const float effectiveScale = std::max(
                    0.001f, canvas->getLocalToDeviceAs3x3().getMaxScale());
                const int pixelWidth = std::max(
                    1, static_cast<int>(std::ceil(
                           op.sourceBounds.width * effectiveScale)));
                const int pixelHeight = std::max(
                    1, static_cast<int>(std::ceil(
                           op.sourceBounds.height * effectiveScale)));
                const auto nearlyEqual = [](float lhs, float rhs) {
                    return std::fabs(lhs - rhs) <= 0.0001f;
                };
                const auto matches = [&](const Impl::CachedLayer& layer) {
                    return layer.pixelWidth == pixelWidth &&
                           layer.pixelHeight == pixelHeight &&
                           nearlyEqual(layer.effectiveScale, effectiveScale) &&
                           nearlyEqual(layer.sourceBounds.x, op.sourceBounds.x) &&
                           nearlyEqual(layer.sourceBounds.y, op.sourceBounds.y) &&
                           nearlyEqual(layer.sourceBounds.width,
                                       op.sourceBounds.width) &&
                           nearlyEqual(layer.sourceBounds.height,
                                       op.sourceBounds.height);
                };

                auto found = m_impl->cachedLayers.find(op.id);
                if (found == m_impl->cachedLayers.end() ||
                    !matches(found->second)) {
                    const SkImageInfo info = SkImageInfo::Make(
                        pixelWidth, pixelHeight, kRGBA_8888_SkColorType,
                        kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
                    auto cachedSurface = m_impl->directContext
                        ? SkSurfaces::RenderTarget(
                              m_impl->directContext.get(),
                              skgpu::Budgeted::kYes, info)
                        : SkSurfaces::Raster(info);
                    if (!cachedSurface) {
                        replaySucceeded = false;
                        return;
                    }
                    Impl::CachedLayer layer{
                        op.sourceBounds, effectiveScale,
                        pixelWidth, pixelHeight, std::move(cachedSurface),
                    };
                    found = m_impl->cachedLayers.insert_or_assign(
                        op.id, std::move(layer)).first;
                    found->second.surface->getCanvas()->clear(
                        SK_ColorTRANSPARENT);
                }

                SkCanvas* cachedCanvas = found->second.surface->getCanvas();
                cachedCanvas->resetMatrix();
                const int cachedCanvasSaveCount = cachedCanvas->getSaveCount();
                cachedCanvas->save();
                cachedCanvas->scale(effectiveScale, effectiveScale);
                cachedCanvas->translate(-op.sourceBounds.x,
                                        -op.sourceBounds.y);
                if (op.updateBounds) {
                    cachedCanvas->clipRect(toSkRect(*op.updateBounds),
                                           SkClipOp::kIntersect, false);
                    cachedCanvas->drawColor(SK_ColorTRANSPARENT,
                                            SkBlendMode::kClear);
                } else {
                    cachedCanvas->drawColor(SK_ColorTRANSPARENT,
                                            SkBlendMode::kClear);
                }

                cachedLayerReplayState = CachedLayerReplayState{
                    canvas, cachedCanvasSaveCount,
                    logicalSaveDepth, logicalLayerDepth,
                };
                canvas = cachedCanvas;
                logicalSaveDepth = 0;
                logicalLayerDepth = 0;
            } else if constexpr (std::is_same_v<T, lcl::graphics::EndCachedLayerCommand>) {
                if (!cachedLayerReplayState) {
                    replaySucceeded = false;
                    return;
                }
                canvas->restoreToCount(
                    cachedLayerReplayState->cachedCanvasSaveCount);
                canvas = cachedLayerReplayState->outerCanvas;
                logicalSaveDepth =
                    cachedLayerReplayState->outerLogicalSaveDepth;
                logicalLayerDepth =
                    cachedLayerReplayState->outerLogicalLayerDepth;
                cachedLayerReplayState.reset();
            } else if constexpr (std::is_same_v<T, lcl::graphics::DrawCachedLayerCommand>) {
                const auto found = m_impl->cachedLayers.find(op.id);
                if (found == m_impl->cachedLayers.end() ||
                    !found->second.surface || op.destination.isEmpty()) {
                    return;
                }
                const auto image = found->second.surface->makeImageSnapshot();
                if (!image) return;
                SkPaint paint;
                paint.setAntiAlias(true);
                paint.setAlphaf(std::clamp(op.opacity, 0.0f, 1.0f));
                canvas->save();
                canvas->concat(toSkMatrix(op.transform));
                canvas->drawImageRect(
                    image, toSkRect(op.destination),
                    SkSamplingOptions(SkFilterMode::kLinear,
                                      SkMipmapMode::kNone),
                    &paint);
                canvas->restore();
            } else if constexpr (std::is_same_v<T, lcl::graphics::DrawPathCommand>) {
                canvas->drawPath(toSkPath(op.path, op.paint.fillRule),
                                 toSkPaint(op.paint));
            } else if constexpr (std::is_same_v<T, lcl::graphics::DrawTextCommand>) {
                const auto prepared = skia_text::prepareFont(
                    op.fontFamily, op.fontSize);
                if (!prepared || op.text.empty()) return;
                SkPaint paint;
                paint.setAntiAlias(true);
                paint.setColor4f(toSkColor(op.color));
                canvas->drawSimpleText(
                    op.text.data(), op.text.size(), SkTextEncoding::kUTF8,
                    op.origin.x, op.origin.y + prepared->metrics.ascent,
                    prepared->font, paint);
            } else if constexpr (std::is_same_v<T, lcl::graphics::DrawImageCommand>) {
                auto image = m_impl->image(op);
                if (!image || op.destination.isEmpty()) return;
                canvas->save();
                if (op.cornerRadius > 0.0f) {
                    lcl::graphics::Path clip;
                    clip.addRRect({op.destination, op.cornerRadius,
                                   op.cornerRadius, op.cornerRoundness});
                    if (op.squareTopCorners) {
                        clip.addRect({op.destination.x, op.destination.y,
                                      op.destination.width,
                                      std::min(op.cornerRadius,
                                               op.destination.height)});
                    }
                    canvas->clipPath(
                        toSkPath(clip, lcl::graphics::FillRule::NonZero),
                        SkClipOp::kIntersect, true);
                }
                SkPaint paint;
                paint.setAntiAlias(true);
                paint.setAlphaf(std::clamp(op.opacity, 0.0f, 1.0f));
                canvas->drawImageRect(
                    image, toSkRect(op.destination),
                    SkSamplingOptions(SkFilterMode::kLinear,
                                      SkMipmapMode::kLinear),
                    &paint);
                canvas->restore();
            } else if constexpr (std::is_same_v<
                    T, lcl::graphics::DrawExternalBufferCommand>) {
                if (op.sampleKind ==
                        lcl::graphics::ExternalBufferSampleKind::Unresolved) {
                    return;
                }
                auto image = m_impl->externalImage(op);
                if (!image || op.destination.isEmpty()) {
                    replaySucceeded = false;
                    return;
                }
                canvas->drawImageRect(
                    image, toSkRect(op.destination),
                    SkSamplingOptions(SkFilterMode::kLinear,
                                      SkMipmapMode::kNone));
            }
        }, command);
    }

    if (cachedLayerReplayState) {
        canvas->restoreToCount(cachedLayerReplayState->cachedCanvasSaveCount);
        canvas = cachedLayerReplayState->outerCanvas;
        cachedLayerReplayState.reset();
        replaySucceeded = false;
    }

    canvas->restoreToCount(replaySaveCount);
    return replaySucceeded;
}

void SkiaDisplayListRenderer::releaseCachedLayer(uint64_t id) {
    m_impl->cachedLayers.erase(id);
}

void SkiaDisplayListRenderer::clearCaches() {
    m_impl->cachedLayers.clear();
    m_impl->imageResources.clear();
}

void SkiaDisplayListRenderer::endFrame() {
    if (!m_impl->surface) return;
    SkCanvas* canvas = m_impl->surface->getCanvas();
    if (m_impl->frameSaveCount > 0) {
        canvas->restoreToCount(m_impl->frameSaveCount);
    }
    if (m_impl->directContext) m_impl->directContext->flushAndSubmit();
    m_impl->frameSaveCount = 0;
}

} // namespace lcl::render
