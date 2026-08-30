#include "core/compositor/compositor_renderer.hpp"
#include "core/compositor/double_inset_border.hpp"
#include "core/compositor/effect_region_geometry.hpp"
#include "core/compositor/mobile_launch_backdrop.hpp"
#include "core/compositor/popup_surface_geometry.hpp"
#include "core/compositor/surface_damage_geometry.hpp"
#include "lcl-theme/theme.hpp"
#include "render/window_group_transform.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace lcl::core {
void CompositorRenderer::render(render::Renderer& renderer,
                                 lcl::platform::IDisplayBackend& displayBackend,
                                 const render::WindowManager& windowManager,
                                 const SurfaceRegistry::Snapshot& surfaces,
                                 const std::function<void()>& beforePresent,
                                 bool allowIncrementalDamage,
                                 bool useMobilePresentation) const {
    using SurfaceEntry = SurfaceRegistry::SurfaceEntry;
    // The snapshot contains only const entry pointers, so protocol/input work
    // cannot mutate the surface state while this frame is being composed.

    auto* raster = renderer.getRasterRenderer();
    const uint64_t resourceGeneration = raster->getResourceGeneration();
    const uint32_t frameWidth = renderer.getWidth();
    const uint32_t frameHeight = renderer.getHeight();
    if (m_retainedFrameResourceGeneration != resourceGeneration ||
        m_retainedFrameWidth != frameWidth ||
        m_retainedFrameHeight != frameHeight) {
        m_hasCompleteRetainedFrame = false;
        m_retainedFrameResourceGeneration = resourceGeneration;
        m_retainedFrameWidth = frameWidth;
        m_retainedFrameHeight = frameHeight;
        m_composedSurfaceFrames.clear();
        m_retainedBackdropBases.clear();
    }
    std::unordered_set<uint32_t> liveWindowIds;
    for (const auto& window : windowManager.getWindows()) {
        liveWindowIds.insert(window.id);
    }
    for (auto cached = m_retainedWindowGroups.begin();
         cached != m_retainedWindowGroups.end();) {
        if (liveWindowIds.contains(cached->first)) {
            ++cached;
            continue;
        }
        if (cached->second.resourceGeneration ==
            raster->getResourceGeneration()) {
            raster->destroyCachedLayerTarget(
                cached->second.framebuffer, cached->second.texture);
        }
        cached = m_retainedWindowGroups.erase(cached);
    }

    std::unordered_set<SurfaceRegistry::Key> liveSurfaceKeys;
    for (const auto& surface : surfaces) {
        if (surface.entry) liveSurfaceKeys.insert(surface.key);
    }
    for (auto composed = m_composedSurfaceFrames.begin();
         composed != m_composedSurfaceFrames.end();) {
        if (liveSurfaceKeys.contains(composed->first)) {
            ++composed;
        } else {
            composed = m_composedSurfaceFrames.erase(composed);
        }
    }
    for (auto cached = m_retainedBackdropBases.begin();
         cached != m_retainedBackdropBases.end();) {
        if (liveSurfaceKeys.contains(cached->first)) {
            ++cached;
            continue;
        }
        if (cached->second.resourceGeneration == resourceGeneration) {
            raster->destroyCachedLayerTarget(
                cached->second.framebuffer, cached->second.texture);
        }
        cached = m_retainedBackdropBases.erase(cached);
    }

    std::optional<graphics::RectF> incrementalDamage;
    std::optional<SurfaceRegistry::Key> incrementalBackdropSurfaceKey;
    const bool hasPendingAtomicConfigure = std::any_of(
        surfaces.begin(), surfaces.end(), [](const auto& surface) {
            return surface.entry &&
                surface.entry->atomicConfigureGeneration != 0;
        });
    // Blur and Glass sample pixels outside their own effect bounds. A normal
    // incremental replay is therefore unsafe. The one bounded exception is a
    // client-only update on the same surface: its already-filtered pre-client
    // backdrop can be restored from an immutable cache before the new layer is
    // composited over it.
    std::unordered_set<SurfaceRegistry::Key> nonLocalEffectSurfaces;
    bool nonLocalEffectsAreBackdropOnly = true;
    for (const auto& surface : surfaces) {
        if (!surface.entry || !surface.entry->hasRenderableBuffer() ||
            surface.entry->pendingDestroy) {
            continue;
        }
        for (const auto& effectRegion : surface.entry->effectRegions) {
            const bool readsNeighbors = std::any_of(
                effectRegion.filters.begin(), effectRegion.filters.end(),
                [](const auto& filter) {
                    return filterReadsNeighboringPixels(filter.type);
                });
            if (!readsNeighbors) continue;
            nonLocalEffectSurfaces.insert(surface.key);
            if (effectRegion.region.source !=
                protocol::EffectSourceType::Backdrop) {
                nonLocalEffectsAreBackdropOnly = false;
            }
        }
    }
    const bool hasNonLocalSceneEffect = !nonLocalEffectSurfaces.empty();
    for (auto cached = m_retainedBackdropBases.begin();
         cached != m_retainedBackdropBases.end();) {
        if (nonLocalEffectSurfaces.contains(cached->first)) {
            ++cached;
            continue;
        }
        if (cached->second.resourceGeneration == resourceGeneration) {
            raster->destroyCachedLayerTarget(
                cached->second.framebuffer, cached->second.texture);
        }
        cached = m_retainedBackdropBases.erase(cached);
    }

    const bool gpuRetainedScene =
        raster->getBackendType() == render::RasterBackend::OpenGL_EGL;
    if (gpuRetainedScene && allowIncrementalDamage &&
        m_hasCompleteRetainedFrame &&
        !hasPendingAtomicConfigure) {
        bool hasDirtyWindow = false;
        bool moveOnly = true;
        graphics::RectF mergedDamage{};
        for (const auto& window : windowManager.getWindows()) {
            if (!window.isDirty) continue;
            hasDirtyWindow = true;
            if (!window.isDragging()) {
                moveOnly = false;
                break;
            }
            mergedDamage = mergedDamage.unionWith(window.damageRect);
        }

        // Popup surfaces may extend beyond their parent WindowGroup. Until
        // popup visual bounds participate in Window damage, keep that less
        // common move path on the correctness-first full redraw.
        if (moveOnly) {
            for (const auto& window : windowManager.getWindows()) {
                if (!window.isDirty) continue;
                SurfaceRegistry::Key parentSurfaceKey = 0;
                for (const auto& surface : surfaces) {
                    if (surface.entry && !surface.entry->isPopup() &&
                        !surface.entry->isAttached() &&
                        surface.entry->windowId == window.id) {
                        parentSurfaceKey = surface.key;
                        break;
                    }
                }
                if (parentSurfaceKey == 0) continue;
                const bool hasPopup = std::any_of(
                    surfaces.begin(), surfaces.end(),
                    [parentSurfaceKey](const auto& surface) {
                        return surface.entry &&
                            surface.entry->parentSurfaceKey == parentSurfaceKey &&
                            surface.entry->hasRenderableBuffer() &&
                            !surface.entry->pendingDestroy;
                    });
                if (hasPopup) {
                    moveOnly = false;
                    break;
                }
            }
        }

        bool hasChangedSurface = false;
        size_t changedSurfaceCount = 0;
        SurfaceRegistry::Key changedSurfaceKey = 0;
        bool contentDamageOnly = true;
        graphics::RectF mergedContentDamage{};
        const graphics::RectF outputBounds{
            0.0f, 0.0f,
            windowManager.getScreenWidth(),
            windowManager.getScreenHeight(),
        };
        for (const auto& surface : surfaces) {
            const auto* entry = surface.entry;
            if (!entry || !entry->hasRenderableBuffer() ||
                entry->pendingDestroy) {
                continue;
            }
            const ComposedSurfaceFrame current{
                entry->frameSerial,
                entry->shmContentSerial,
                entry->rasterLayerId,
            };
            const auto previous = m_composedSurfaceFrames.find(surface.key);
            const bool changed = previous == m_composedSurfaceFrames.end() ||
                previous->second.frameSerial != current.frameSerial ||
                previous->second.shmContentSerial != current.shmContentSerial ||
                previous->second.rasterLayerId != current.rasterLayerId;
            if (!changed) continue;
            hasChangedSurface = true;
            ++changedSurfaceCount;
            changedSurfaceKey = surface.key;

            // Begin narrowly with regular parent surfaces. Attached and popup
            // geometry can extend outside the parent content destination and
            // therefore requires its own dependency closure.
            if (entry->isAttached() || entry->isPopup() ||
                entry->atomicConfigureGeneration != 0 ||
                entry->shmDamageWidth == 0 ||
                entry->shmDamageHeight == 0 ||
                entry->launchMorphActive ||
                (useMobilePresentation &&
                 entry->systemSurfaceKind !=
                     protocol::LCLSystemSurfaceKind::None) ||
                std::fabs(entry->transitionOpacity - 1.0f) > 0.0001f ||
                std::fabs(entry->transitionScale - 1.0f) > 0.0001f) {
                contentDamageOnly = false;
                break;
            }

            const auto window = std::find_if(
                windowManager.getWindows().begin(),
                windowManager.getWindows().end(),
                [entry](const auto& candidate) {
                    return candidate.id == entry->windowId;
                });
            if (window == windowManager.getWindows().end() ||
                window->isMinimized) {
                contentDamageOnly = false;
                break;
            }

            const float titleOffset =
                window->decorationMode == render::DecorationMode::SSD
                    ? 32.0f : 0.0f;
            const auto group = render::makeWindowGroupTransform(
                *window, titleOffset, 1.0f);
            graphics::RectF destination = group.globalBounds;
            if (window->decorationMode == render::DecorationMode::SSD) {
                destination.y += group.titleHeight;
                destination.height = std::max(
                    1.0f, destination.height - group.titleHeight);
            }
            const float scale = std::max(0.001f, entry->bufferScale);
            if (std::fabs(destination.width * scale - entry->width) > 1.0f ||
                std::fabs(destination.height * scale - entry->height) > 1.0f) {
                // The normal contract is exact geometry. A defensive cropped
                // presentation cannot safely use normalized damage mapping.
                contentDamageOnly = false;
                break;
            }
            const auto mapped = mapSurfaceDamageToDestination(
                entry->width, entry->height,
                entry->shmDamageX, entry->shmDamageY,
                entry->shmDamageWidth, entry->shmDamageHeight,
                destination, outputBounds);
            if (!mapped) {
                contentDamageOnly = false;
                break;
            }
            mergedContentDamage = mergedContentDamage.unionWith(*mapped);
        }

        if (hasDirtyWindow && moveOnly && !hasChangedSurface &&
            !hasNonLocalSceneEffect &&
            !mergedDamage.isEmpty()) {
            // Cover antialiased window edges and fractional motion before
            // clipping to the logical output. There are currently no exterior
            // compositor shadows; if one is introduced its radius must be
            // included here as part of the WindowGroup visual bounds.
            constexpr float kEdgeSafety = 2.0f;
            const graphics::RectF expanded{
                mergedDamage.x - kEdgeSafety,
                mergedDamage.y - kEdgeSafety,
                mergedDamage.width + kEdgeSafety * 2.0f,
                mergedDamage.height + kEdgeSafety * 2.0f,
            };
            const auto clipped = expanded.intersection(outputBounds);
            if (!clipped.isEmpty()) incrementalDamage = clipped;
        } else if (!hasDirtyWindow && hasChangedSurface &&
                   contentDamageOnly && !mergedContentDamage.isEmpty()) {
            const bool ordinaryDamage = !hasNonLocalSceneEffect;
            bool cachedBackdropDamage = false;
            if (!ordinaryDamage &&
                nonLocalEffectSurfaces.contains(changedSurfaceKey)) {
                const auto changedSurface = std::find_if(
                    surfaces.begin(), surfaces.end(),
                    [changedSurfaceKey](const auto& candidate) {
                        return candidate.key == changedSurfaceKey;
                    });
                const auto cached =
                    m_retainedBackdropBases.find(changedSurfaceKey);
                if (changedSurface != surfaces.end() &&
                    changedSurface->entry &&
                    cached != m_retainedBackdropBases.end()) {
                    const auto& entry = *changedSurface->entry;
                    const auto window = std::find_if(
                        windowManager.getWindows().begin(),
                        windowManager.getWindows().end(),
                        [&entry](const auto& candidate) {
                            return candidate.id == entry.windowId;
                        });
                    if (window != windowManager.getWindows().end()) {
                        const float titleOffset =
                            window->decorationMode ==
                                render::DecorationMode::SSD
                                ? 32.0f : 0.0f;
                        const auto group = render::makeWindowGroupTransform(
                            *window, titleOffset, entry.transitionScale);
                        const auto expectedBounds =
                            group.globalBounds.intersection(outputBounds);
                        const auto& base = cached->second;
                        const auto same = [](float lhs, float rhs) {
                            return std::fabs(lhs - rhs) <= 0.0001f;
                        };
                        const bool retainedBackdropMatches = base.valid &&
                            base.resourceGeneration == resourceGeneration &&
                            base.effectRevision == entry.effectRevision &&
                            same(base.bounds.x, expectedBounds.x) &&
                            same(base.bounds.y, expectedBounds.y) &&
                            same(base.bounds.width, expectedBounds.width) &&
                            same(base.bounds.height, expectedBounds.height) &&
                            same(base.windowOpacity, entry.transitionOpacity) &&
                            same(base.windowScale, entry.transitionScale);
                        cachedBackdropDamage =
                            canReuseRetainedBackdropForClientDamage(
                                changedSurfaceCount,
                                nonLocalEffectSurfaces.size(),
                                nonLocalEffectsAreBackdropOnly,
                                true,
                                retainedBackdropMatches);
                    }
                }
            }
            if (ordinaryDamage || cachedBackdropDamage) {
                incrementalDamage = mergedContentDamage;
                if (cachedBackdropDamage) {
                    incrementalBackdropSurfaceKey = changedSurfaceKey;
                }
            }
        }
    }

    // All GPU platforms retain one authoritative compositor scene. Rotating
    // output buffers are still populated by RasterRenderer::endFrame; only
    // scene composition is clipped here.
    raster->setRetainsFrameBacking(gpuRetainedScene);

    // --- Begin LCL raster frame ---
    raster->beginFrame();
    // The scene backing remains authoritative across presentation. Carry the
    // same bounded compositor damage to the final platform target so a
    // rotating output buffer can catch up with only the patches it missed.
    if (incrementalDamage) {
        raster->setOutputFrameDamageRect(render::RasterRect{
            incrementalDamage->x, incrementalDamage->y,
            incrementalDamage->width, incrementalDamage->height});
    } else {
        // A full composition, a transition, or an effect with nonlocal reads
        // must refresh the entire final target.
        raster->setOutputFrameDamageRect(std::nullopt);
    }

    constexpr float kWindowCornerRadiusLogical = 20.0f;

    const float outputScale = raster->getDeviceScale();
    const graphics::RectF outputBounds{
        0.0f, 0.0f,
        windowManager.getScreenWidth(),
        windowManager.getScreenHeight(),
    };
    auto drawRetainedBackdropBase = [&](SurfaceRegistry::Key surfaceKey) {
        const auto cached = m_retainedBackdropBases.find(surfaceKey);
        if (cached == m_retainedBackdropBases.end() ||
            !cached->second.valid || cached->second.texture == 0 ||
            cached->second.resourceGeneration !=
                raster->getResourceGeneration()) {
            return false;
        }
        raster->drawCachedLayerTexture(
            cached->second.texture,
            {cached->second.bounds.x, cached->second.bounds.y,
             cached->second.bounds.width, cached->second.bounds.height});
        return true;
    };
    auto retainBackdropBase = [&](SurfaceRegistry::Key surfaceKey,
                                  const graphics::RectF& visualBounds,
                                  uint64_t effectRevision,
                                  float windowOpacity,
                                  float windowScale) {
        if (!gpuRetainedScene) return;
        const auto clipped = visualBounds.intersection(outputBounds);
        if (clipped.isEmpty()) return;
        const uint32_t pixelWidth = std::max(
            1u, static_cast<uint32_t>(std::ceil(clipped.width * outputScale)));
        const uint32_t pixelHeight = std::max(
            1u, static_cast<uint32_t>(std::ceil(clipped.height * outputScale)));
        auto& cached = m_retainedBackdropBases[surfaceKey];
        if (cached.resourceGeneration != raster->getResourceGeneration()) {
            cached = {};
            cached.resourceGeneration = raster->getResourceGeneration();
        }
        const bool recreate = cached.pixelWidth != pixelWidth ||
            cached.pixelHeight != pixelHeight || cached.framebuffer == 0 ||
            cached.texture == 0;
        if (recreate) {
            raster->destroyCachedLayerTarget(
                cached.framebuffer, cached.texture);
            cached.framebuffer = 0;
            cached.texture = 0;
            cached.valid = false;
            if (!raster->createCachedLayerTarget(
                    pixelWidth, pixelHeight,
                    cached.framebuffer, cached.texture)) {
                return;
            }
        }
        cached.pixelWidth = pixelWidth;
        cached.pixelHeight = pixelHeight;
        cached.bounds = clipped;
        cached.effectRevision = effectRevision;
        cached.windowOpacity = windowOpacity;
        cached.windowScale = windowScale;
        cached.valid = raster->copyFrameRegionToCachedLayer(
            cached.framebuffer, cached.texture, nullptr,
            pixelWidth, pixelHeight,
            {clipped.x, clipped.y, clipped.width, clipped.height});
    };
    auto groupHasPendingAtomicConfigure = [&](uint32_t windowId) {
        return std::any_of(
            surfaces.begin(), surfaces.end(), [windowId](const auto& surface) {
                return surface.entry && surface.entry->windowId == windowId &&
                    surface.entry->atomicConfigureGeneration != 0;
            });
    };
    auto drawRetainedWindowGroup = [&](const RetainedWindowGroup& cached) {
        if (!cached.valid || cached.bounds.isEmpty() ||
            cached.resourceGeneration != raster->getResourceGeneration()) {
            return false;
        }
        if (cached.texture != 0) {
            raster->drawDmaBufTextureTransformed(
                cached.bounds.x, cached.bounds.y,
                static_cast<int>(cached.pixelWidth),
                static_cast<int>(cached.pixelHeight),
                static_cast<int>(cached.pixelWidth),
                static_cast<int>(cached.pixelHeight),
                cached.texture, 1.0f,
                cached.cornerRadius, cached.cornerRoundness, false,
                cached.bounds.width, cached.bounds.height);
            return true;
        }
        if (cached.pixels.empty()) return false;
        raster->drawBufferTransformed(
            cached.bounds.x, cached.bounds.y,
            static_cast<int>(cached.pixelWidth),
            static_cast<int>(cached.pixelHeight),
            cached.pixels.data(), static_cast<int>(cached.pixelWidth),
            1.0f, cached.cornerRadius, cached.cornerRoundness, false,
            cached.bounds.width, cached.bounds.height);
        return true;
    };
    auto retainWindowGroup = [&](uint32_t windowId,
                                 const graphics::RectF& visualBounds,
                                 float cornerRadius,
                                 float cornerRoundness) {
        const auto clipped = visualBounds.intersection(outputBounds);
        if (clipped.isEmpty()) return;

        const uint32_t pixelWidth = std::max(
            1u, static_cast<uint32_t>(std::ceil(clipped.width * outputScale)));
        const uint32_t pixelHeight = std::max(
            1u, static_cast<uint32_t>(std::ceil(clipped.height * outputScale)));
        auto& cached = m_retainedWindowGroups[windowId];
        const bool gpu =
            raster->getBackendType() == render::RasterBackend::OpenGL_EGL;
        if (cached.resourceGeneration != raster->getResourceGeneration()) {
            // Numeric GL names from a destroyed context must never be deleted
            // or sampled in its replacement context.
            cached = {};
            cached.resourceGeneration = raster->getResourceGeneration();
        }
        const bool boundsChanged =
            std::fabs(cached.bounds.x - clipped.x) > 0.0001f ||
            std::fabs(cached.bounds.y - clipped.y) > 0.0001f ||
            std::fabs(cached.bounds.width - clipped.width) > 0.0001f ||
            std::fabs(cached.bounds.height - clipped.height) > 0.0001f;
        const bool recreate = cached.pixelWidth != pixelWidth ||
            cached.pixelHeight != pixelHeight ||
            (gpu && (cached.framebuffer == 0 || cached.texture == 0)) ||
            (!gpu && (cached.framebuffer != 0 || cached.texture != 0));
        if (recreate) {
            raster->destroyCachedLayerTarget(
                cached.framebuffer, cached.texture);
            cached.framebuffer = 0;
            cached.texture = 0;
            cached.pixels.clear();
            cached.valid = false;
            if (gpu && !raster->createCachedLayerTarget(
                    pixelWidth, pixelHeight,
                    cached.framebuffer, cached.texture)) {
                return;
            }
        }
        cached.pixelWidth = pixelWidth;
        cached.pixelHeight = pixelHeight;
        cached.bounds = clipped;
        cached.cornerRadius = cornerRadius;
        cached.cornerRoundness = cornerRoundness;
        if (gpu) {
            cached.pixels.clear();
        } else {
            cached.pixels.resize(
                static_cast<size_t>(pixelWidth) * pixelHeight);
        }
        if (incrementalDamage && cached.valid && !recreate &&
            !boundsChanged) {
            if (clipped.intersects(*incrementalDamage)) {
                cached.valid = raster->copyFrameDamageToCachedLayer(
                    cached.framebuffer, cached.texture,
                    cached.pixels.empty() ? nullptr : cached.pixels.data(),
                    pixelWidth, pixelHeight,
                    {clipped.x, clipped.y, clipped.width, clipped.height},
                    {incrementalDamage->x, incrementalDamage->y,
                     incrementalDamage->width, incrementalDamage->height});
            }
            return;
        }
        cached.valid = raster->copyFrameRegionToCachedLayer(
            cached.framebuffer, cached.texture,
            cached.pixels.empty() ? nullptr : cached.pixels.data(),
            pixelWidth, pixelHeight,
            {clipped.x, clipped.y, clipped.width, clipped.height});
    };
    auto drawShmSurface = [&](SurfaceRegistry::Key cacheKey,
                              uint64_t contentSerial,
                              float dstX, float dstY,
                              int srcW, int srcH, int backingW, int backingH,
                              const uint32_t* pixels, int stridePixels,
                              int damageX, int damageY, int damageW, int damageH,
                              float opacity, float cornerRadius,
                              float cornerRoundness, bool squareTopCorners,
                              float drawWidth, float drawHeight,
                              render::RasterBufferSampling sampling =
                                  render::RasterBufferSampling::Stretch) {
#if defined(__ANDROID__)
        raster->drawCachedShmBufferTransformed(
            cacheKey, contentSerial, dstX, dstY, srcW, srcH,
            backingW, backingH, pixels, stridePixels,
            damageX, damageY, damageW, damageH,
            opacity, cornerRadius, cornerRoundness,
            squareTopCorners, drawWidth, drawHeight, sampling);
#else
        (void)cacheKey;
        (void)contentSerial;
        (void)backingW;
        (void)backingH;
        (void)damageX;
        (void)damageY;
        (void)damageW;
        (void)damageH;
        raster->drawBufferTransformed(
            dstX, dstY, srcW, srcH, pixels, stridePixels, opacity,
            cornerRadius, cornerRoundness, squareTopCorners,
            drawWidth, drawHeight, sampling);
#endif
    };
    auto resolveWindowCornerRadiusLogical = [&](const render::Window& win) {
        if (win.cornerRadius >= 0.0f) {
            return win.cornerRadius;
        }

        if (win.decorationMode == render::DecorationMode::SSD) {
            return kWindowCornerRadiusLogical;
        }

        if (win.decorationMode == render::DecorationMode::None &&
            win.title.find("Terminal") != std::string::npos) {
            return kWindowCornerRadiusLogical;
        }

        return 0.0f;
    };
    auto resolveWindowCornerRoundness = [](const render::Window& win) {
        return std::clamp(win.cornerRoundness, 2.0f, 8.0f);
    };

    auto replayLogicalList = [&](const graphics::DisplayList& displayList,
                                 const graphics::Matrix3& rootTransform = {}) {
        raster->replayDisplayList(displayList,
            {{static_cast<float>(renderer.getWidth()) / outputScale,
              static_cast<float>(renderer.getHeight()) / outputScale},
             {renderer.getWidth(), renderer.getHeight()}, outputScale},
            rootTransform);
    };
    const SurfaceEntry* homeScreenSurface = nullptr;
    for (const auto& surface : surfaces) {
        if (surface.entry &&
            surface.entry->systemSurfaceKind ==
                protocol::LCLSystemSurfaceKind::HomeScreen &&
            surface.entry->hasRenderableBuffer()) {
            homeScreenSurface = surface.entry;
            break;
        }
    }

    float mobileLaunchProgress = 0.0f;
    if (useMobilePresentation) {
        // WindowManager order is bottom-to-top. The uppermost visible normal
        // surface is the sole authority for launcher coverage; minimized apps
        // must not keep Home dimmed behind the launcher.
        for (auto winIt = windowManager.getWindows().rbegin();
             winIt != windowManager.getWindows().rend(); ++winIt) {
            if (winIt->isMinimized) continue;
            const auto surface = std::find_if(
                surfaces.begin(), surfaces.end(), [&](const auto& item) {
                    return item.entry && !item.entry->isPopup() &&
                        !item.entry->isAttached() &&
                        item.entry->windowId == winIt->id &&
                        item.entry->systemSurfaceKind ==
                            protocol::LCLSystemSurfaceKind::None;
                });
            if (surface == surfaces.end()) continue;
            if (surface->entry->hasLaunchOrigin &&
                surface->entry->launchToken != 0) {
                mobileLaunchProgress =
                    surface->entry->launchHomeTransitionProgress;
            } else {
                mobileLaunchProgress = 1.0f;
            }
            break;
        }
    }
    const auto mobileBackdrop =
        resolveMobileLaunchBackdrop(mobileLaunchProgress);

    auto drawLaunchIconProxy = [&](const SurfaceEntry& surface) {
        if (!surface.hasLaunchOrigin ||
            !surface.launchMorphActive || surface.transitionOpacity >= 0.999f) {
            return;
        }
        const size_t snapshotPixelCount =
            static_cast<size_t>(surface.launchIconWidth) *
            surface.launchIconHeight;
        if (surface.launchIconWidth > 0 && surface.launchIconHeight > 0 &&
            surface.launchIconPixels.size() == snapshotPixelCount) {
            const uint64_t iconCacheKey =
                (uint64_t{1} << 63) | surface.launchToken;
            raster->drawCachedShmBufferTransformed(
                iconCacheKey, 1,
                surface.launchMorphX, surface.launchMorphY,
                static_cast<int>(surface.launchIconWidth),
                static_cast<int>(surface.launchIconHeight),
                static_cast<int>(surface.launchIconWidth),
                static_cast<int>(surface.launchIconHeight),
                surface.launchIconPixels.data(),
                static_cast<int>(surface.launchIconWidth),
                0, 0,
                static_cast<int>(surface.launchIconWidth),
                static_cast<int>(surface.launchIconHeight),
                1.0f,
                surface.launchMorphCornerRadius,
                surface.launchMorphCornerRoundness, false,
                surface.launchMorphWidth, surface.launchMorphHeight,
                render::RasterBufferSampling::
                    TopLeftAnchoredExtendTrailingEdge);
            return;
        }
        if (!homeScreenSurface) return;
        const auto& home = *homeScreenSurface;
        const float scale = std::max(0.001f, home.bufferScale);
        const int sourceX = static_cast<int>(std::lround(
            (surface.launchOriginX - home.configuredX) * scale));
        const int sourceY = static_cast<int>(std::lround(
            (surface.launchOriginY - home.configuredY) * scale));
        const int sourceWidth = std::max(1, static_cast<int>(std::lround(
            surface.launchOriginWidth * scale)));
        const int sourceHeight = std::max(1, static_cast<int>(std::lround(
            surface.launchOriginHeight * scale)));
        if (sourceX < 0 || sourceY < 0 ||
            sourceX + sourceWidth > static_cast<int>(home.width) ||
            sourceY + sourceHeight > static_cast<int>(home.height)) {
            return;
        }

        if (home.pixels) {
            const int stridePixels = static_cast<int>(home.stride / 4);
            const auto* pixels = reinterpret_cast<const uint32_t*>(home.pixels) +
                static_cast<size_t>(sourceY) * stridePixels + sourceX;
            raster->drawBufferTransformed(
                surface.launchMorphX, surface.launchMorphY,
                sourceWidth, sourceHeight, pixels, stridePixels, 1.0f,
                surface.launchMorphCornerRadius,
                surface.launchMorphCornerRoundness, false,
                surface.launchMorphWidth, surface.launchMorphHeight,
                render::RasterBufferSampling::
                    TopLeftAnchoredExtendTrailingEdge);
        } else if (home.rasterLayerTexture != 0) {
            raster->drawDmaBufTextureRegionTransformed(
                surface.launchMorphX, surface.launchMorphY,
                surface.launchMorphWidth, surface.launchMorphHeight,
                static_cast<int>(home.width),
                static_cast<int>(home.height),
                static_cast<int>(home.backingWidth),
                static_cast<int>(home.backingHeight),
                sourceX, sourceY, sourceWidth, sourceHeight,
                home.rasterLayerTexture, 1.0f,
                surface.launchMorphCornerRadius,
                surface.launchMorphCornerRoundness);
        }
    };

    // 1. Clear either the complete scene or only the merged move/content damage.
    if (incrementalDamage) {
        const render::RasterRect damage{
            incrementalDamage->x, incrementalDamage->y,
            incrementalDamage->width, incrementalDamage->height};
        raster->setFrameDamageRect(damage);
        raster->clearRect(damage, {0, 0, 0, 255});
    } else {
        raster->setFrameDamageRect(std::nullopt);
        raster->setClipRect(std::nullopt);
        raster->clear({0, 0, 0, 255});
        m_hasCompleteRetainedFrame = true;
    }

    // 2. Atomic Z-Stacking Window Group Rendering (Frame + Client Surface per Window in Z-order)
    auto applySurfaceRegionEffects = [&](const render::Window& win,
                                         const SurfaceEntry& surface,
                                         protocol::EffectSourceType sourceType,
                                         float windowOpacity,
        const render::WindowGroupTransform& group) {
        const float titleOffset = (win.decorationMode == render::DecorationMode::SSD)
            ? 32.0f
            : 0.0f;

        for (const auto& fx : surface.effectRegions) {
            if (fx.region.source != sourceType || fx.filters.empty()) continue;

            const bool outerSurface =
                fx.region.boundsPolicy == protocol::EffectBoundsPolicy::OuterSurface;
            float fxX = 0.0f;
            float fxY = 0.0f;
            float fxW = 0.0f;
            float fxH = 0.0f;
            float cornerRadius = 0.0f;
            float cornerRoundness = 2.0f;
            if (outerSurface) {
                // The compositor owns the outer surface geometry, including
                // desktop or mobile system insets. A root backdrop therefore
                // cannot drift below it or select a different corner shape.
                fxX = group.globalBounds.x;
                fxY = group.globalBounds.y;
                fxW = std::max(1.0f, group.globalBounds.width);
                fxH = std::max(1.0f, group.globalBounds.height);
                cornerRadius = surface.launchMorphActive
                    ? surface.launchMorphCornerRadius
                    : group.mapLength(resolveWindowCornerRadiusLogical(win));
                cornerRoundness = surface.launchMorphActive
                    ? surface.launchMorphCornerRoundness
                    : resolveWindowCornerRoundness(win);
            } else {
                const auto local = resolveLocalEffectGeometry(
                    0.0f, 0.0f, titleOffset,
                    std::max(1.0f, group.localBounds.width),
                    std::max(1.0f, group.localBounds.height - titleOffset),
                    fx.region, fx.followSurfaceBounds);
                const auto mapped = group.mapRect(
                    {local.x, local.y, local.width, local.height});
                fxX = mapped.x;
                fxY = mapped.y;
                fxW = mapped.width;
                fxH = mapped.height;
                cornerRadius = group.mapLength(
                    std::max(0.0f, fx.region.cornerRadius));
                cornerRoundness = std::clamp(fx.region.cornerRoundness, 2.0f, 8.0f);
            }
            if (fxW <= 0.0f || fxH <= 0.0f) continue;
            if (incrementalDamage && !incrementalDamage->intersects(
                    {fxX, fxY, fxW, fxH})) {
                continue;
            }

            // Initial executor supports chain filters with source-type routing.
            // Advanced blend modes are currently treated as normal blend.
            renderer.getRasterRenderer()->applyBackdropFilter(
                fxX, fxY, fxW, fxH,
                cornerRadius,
                cornerRoundness,
                std::clamp(fx.region.opacity * windowOpacity, 0.0f, 1.0f),
                fx.filters);
        }
    };

    for (const auto& win : windowManager.getWindows()) {
        if (win.isMinimized) {
            continue;
        }
        // A. Find matching client SHM surface buffer for this window
        const SurfaceEntry* matchingSurface = nullptr;
        SurfaceRegistry::Key matchingSurfaceKey = 0;
        for (const auto& surface : surfaces) {
            const auto* entry = surface.entry;
            if (entry && !entry->isPopup() && !entry->isAttached() &&
                entry->windowId == win.id &&
                (entry->hasRenderableBuffer() ||
                 entry->launchPlaceholderActive ||
                 entry->isLaunchPlaceholder)) {
                matchingSurface = entry;
                matchingSurfaceKey = surface.key;
                break;
            }
        }

        const float titleOffset = (win.decorationMode == render::DecorationMode::SSD)
            ? 32.0f
            : 0.0f;

        float windowOpacity = 1.0f;
        float windowScale = 1.0f;
        if (matchingSurface) {
            windowOpacity = std::clamp(matchingSurface->transitionOpacity, 0.0f, 1.0f);
            windowScale = std::clamp(matchingSurface->transitionScale, 0.80f, 1.20f);
            if (useMobilePresentation) {
                if (matchingSurface->systemSurfaceKind ==
                    protocol::LCLSystemSurfaceKind::Wallpaper) {
                    windowScale = mobileBackdrop.wallpaperScale;
                } else if (matchingSurface->systemSurfaceKind ==
                           protocol::LCLSystemSurfaceKind::HomeScreen) {
                    windowScale = mobileBackdrop.homeScale;
                }
            }
        }

        const render::WindowGroupTransform group =
            matchingSurface && matchingSurface->launchMorphActive
                ? render::makeWindowGroupTransformToBounds(
                    win, titleOffset,
                    {matchingSurface->launchMorphX,
                     matchingSurface->launchMorphY,
                     matchingSurface->launchMorphWidth,
                     matchingSurface->launchMorphHeight})
                : render::makeWindowGroupTransform(win, titleOffset, windowScale);

        // Atomicity is local to this WindowGroup. Keep presenting the last
        // complete composited group while its parent and geometry-following
        // attachments commit the next epoch; unrelated windows and shell
        // transitions continue to advance and present every frame.
        const bool atomicConfigurePending =
            groupHasPendingAtomicConfigure(win.id);
        if (atomicConfigurePending) {
            const auto cached = m_retainedWindowGroups.find(win.id);
            if (cached != m_retainedWindowGroups.end() &&
                drawRetainedWindowGroup(cached->second)) {
                continue;
            }
        }

        const graphics::RectF groupVisualBounds = group.globalBounds;

        // B. Apply effect-graph backdrop regions. When only this client's
        // retained layer changed, restore the already-filtered pre-client base
        // instead of executing an unchanged full-surface blur again.
        const bool reusingBackdropBase = matchingSurface &&
            incrementalBackdropSurfaceKey &&
            *incrementalBackdropSurfaceKey == matchingSurfaceKey;
        const bool hasNonLocalBackdrop = matchingSurface && std::any_of(
            matchingSurface->effectRegions.begin(),
            matchingSurface->effectRegions.end(), [](const auto& region) {
                return region.region.source ==
                        protocol::EffectSourceType::Backdrop &&
                    std::any_of(
                        region.filters.begin(), region.filters.end(),
                        [](const auto& filter) {
                            return filterReadsNeighboringPixels(filter.type);
                        });
            });
        if (reusingBackdropBase) {
            (void)drawRetainedBackdropBase(matchingSurfaceKey);
        } else if (matchingSurface &&
                   matchingSurface->hasRenderableBuffer() &&
                   !matchingSurface->effectRegions.empty()) {
            applySurfaceRegionEffects(
                win, *matchingSurface,
                protocol::EffectSourceType::Backdrop,
                windowOpacity, group);
            if (!incrementalDamage && hasNonLocalBackdrop) {
                retainBackdropBase(
                    matchingSurfaceKey, group.globalBounds,
                    matchingSurface->effectRevision,
                    windowOpacity, windowScale);
            }
        }

        if (matchingSurface) {
            // The launcher owns the icon asset and supplies one immutable
            // snapshot. The compositor only retains and transforms that small
            // buffer, so hiding the stationary widget cannot erase the proxy.
            drawLaunchIconProxy(*matchingSurface);

            int srcW = static_cast<int>(matchingSurface->width);
            int srcH = static_cast<int>(matchingSurface->height);
            int stridePixels = static_cast<int>(matchingSurface->stride / 4);
            float drawX = group.globalBounds.x;
            float drawY = group.globalBounds.y;
            float drawW = group.globalBounds.width;
            float drawH = group.globalBounds.height;

            if (win.decorationMode == render::DecorationMode::SSD) {
                // The client buffer occupies the remaining exact pixels of
                // the same transformed group rect used by an attached frame.
                drawY += group.titleHeight;
                drawH = std::max(1.0f, group.globalBounds.height - group.titleHeight);
            }

            const float windowCornerRadius = matchingSurface->launchMorphActive
                ? matchingSurface->launchMorphCornerRadius
                : resolveWindowCornerRadiusLogical(win);
            const bool maskToWindowShape = windowCornerRadius > 0.001f;
            const float presentedCornerRadius = matchingSurface->launchMorphActive
                ? windowCornerRadius
                : group.mapLength(windowCornerRadius);
            const float presentedCornerRoundness =
                matchingSurface->launchMorphActive
                ? matchingSurface->launchMorphCornerRoundness
                : resolveWindowCornerRoundness(win);
            // Application layers are never resized by sampling. A matching
            // geometry generation has identical extents; a defensive mismatch
            // is cropped at the trailing edge instead of stretched.
            constexpr render::RasterBufferSampling contentSampling =
                render::RasterBufferSampling::TopLeftAnchoredCropTrailingEdge;

            if (matchingSurface->launchPlaceholderActive) {
                raster->drawRoundedRect(
                    {drawX, drawY, drawW, drawH},
                    maskToWindowShape ? presentedCornerRadius : 0.0f,
                    {255, 255, 255,
                     static_cast<uint8_t>(std::clamp(
                         std::lround(windowOpacity * 255.0f), 0l, 255l))},
                    {}, 0.0f, presentedCornerRoundness);
            }

            const float contentOpacity = windowOpacity * std::clamp(
                matchingSurface->launchContentOpacity, 0.0f, 1.0f);

            const float currentOpacity = contentOpacity;
            if (matchingSurface->pixels) {
                drawShmSurface(
                    matchingSurfaceKey, matchingSurface->shmContentSerial,
                    drawX, drawY, srcW, srcH,
                    static_cast<int>(matchingSurface->backingWidth),
                    static_cast<int>(matchingSurface->backingHeight),
                    reinterpret_cast<const uint32_t*>(matchingSurface->pixels),
                    stridePixels,
                    static_cast<int>(matchingSurface->shmDamageX),
                    static_cast<int>(matchingSurface->shmDamageY),
                    static_cast<int>(matchingSurface->shmDamageWidth),
                    static_cast<int>(matchingSurface->shmDamageHeight),
                    currentOpacity,
                    maskToWindowShape ? presentedCornerRadius : 0.0f,
                    presentedCornerRoundness,
                    win.decorationMode == render::DecorationMode::SSD,
                    drawW, drawH, contentSampling);
            } else if (matchingSurface->rasterLayerTexture != 0) {
                raster->drawDmaBufTextureTransformed(
                    drawX, drawY, srcW, srcH,
                    static_cast<int>(matchingSurface->backingWidth),
                    static_cast<int>(matchingSurface->backingHeight),
                    matchingSurface->rasterLayerTexture,
                    currentOpacity,
                    maskToWindowShape ? presentedCornerRadius : 0.0f,
                    presentedCornerRoundness,
                    win.decorationMode == render::DecorationMode::SSD,
                    drawW, drawH, contentSampling);
            }

            if (matchingSurface->hasRenderableBuffer() &&
                !matchingSurface->effectRegions.empty()) {
                applySurfaceRegionEffects(win, *matchingSurface, protocol::EffectSourceType::Foreground,
                                          windowOpacity, group);
            }

            if (useMobilePresentation &&
                matchingSurface->systemSurfaceKind ==
                    protocol::LCLSystemSurfaceKind::HomeScreen &&
                mobileBackdrop.progress > 0.0001f) {
                protocol::FilterOp brightness{};
                brightness.type = protocol::FilterType::Brightness;
                brightness.value = mobileBackdrop.brightness;

                // Launch progress changes brightness directly; it never becomes
                // effect opacity.
                raster->applyBackdropFilter(
                    0.0f, 0.0f,
                    windowManager.getScreenWidth(),
                    windowManager.getScreenHeight(),
                    0.0f, 2.0f, 1.0f,
                    {brightness});
            }

        }

        // C. Compose generic WM-owned children in the same WindowGroup. The
        // compositor knows attachment geometry, ordering and input policy, not
        // whether the pixels represent a frame, adornment, or future chrome.
        std::vector<SurfaceRegistry::SnapshotEntry> attachments;
        for (const auto& surface : surfaces) {
            const auto* attached = surface.entry;
            if (attached && attached->isAttached() &&
                attached->attachedWindowId == win.id &&
                attached->hasCommittedBuffer &&
                attached->hasRenderableBuffer() &&
                !attached->pendingDestroy) {
                attachments.push_back(surface);
            }
        }
        std::sort(attachments.begin(), attachments.end(),
                  [](const auto& lhs, const auto& rhs) {
            if (lhs.entry->attachedRole != rhs.entry->attachedRole) {
                return static_cast<uint32_t>(lhs.entry->attachedRole) <
                    static_cast<uint32_t>(rhs.entry->attachedRole);
            }
            return lhs.entry->attachmentOrder < rhs.entry->attachmentOrder;
        });
        for (const auto& attachedSurface : attachments) {
            const auto* attached = attachedSurface.entry;
            constexpr render::RasterBufferSampling attachmentSampling =
                render::RasterBufferSampling::TopLeftAnchoredCropTrailingEdge;
            const graphics::RectF localBounds{
                attached->attachedX,
                attached->attachedY,
                attached->attachedFollowParentWidth
                    ? group.localBounds.width : attached->attachedWidth,
                attached->attachedFollowParentHeight
                    ? group.localBounds.height : attached->attachedHeight,
            };
            const auto bounds = group.mapRect(localBounds);
            const float opacity = windowOpacity * std::clamp(
                attached->transitionOpacity, 0.0f, 1.0f);
            if (attached->pixels) {
                drawShmSurface(
                    attachedSurface.key, attached->shmContentSerial,
                    bounds.x, bounds.y,
                    static_cast<int>(attached->width),
                    static_cast<int>(attached->height),
                    static_cast<int>(attached->backingWidth),
                    static_cast<int>(attached->backingHeight),
                    reinterpret_cast<const uint32_t*>(attached->pixels),
                    static_cast<int>(attached->stride / 4),
                    static_cast<int>(attached->shmDamageX),
                    static_cast<int>(attached->shmDamageY),
                    static_cast<int>(attached->shmDamageWidth),
                    static_cast<int>(attached->shmDamageHeight),
                    opacity, 0.0f, 2.0f, false,
                    bounds.width, bounds.height, attachmentSampling);
            } else if (attached->rasterLayerTexture != 0) {
                raster->drawDmaBufTextureTransformed(
                    bounds.x, bounds.y,
                    static_cast<int>(attached->width),
                    static_cast<int>(attached->height),
                    static_cast<int>(attached->backingWidth),
                    static_cast<int>(attached->backingHeight),
                    attached->rasterLayerTexture, opacity,
                    0.0f, 2.0f, false,
                    bounds.width, bounds.height, attachmentSampling);
            }
        }

        // Forced compositor-owned inset border for every window, independent from app UI.
        if (win.drawInsetBorder) {
            float borderRadius = resolveWindowCornerRadiusLogical(win);
            if (borderRadius <= 0.001f) {
                borderRadius = kWindowCornerRadiusLogical;
            }
            const graphics::RectF borderBounds = group.localBounds;
            replayLogicalList(
                buildDoubleInsetBorderDisplayList(
                    borderBounds, borderRadius,
                    resolveWindowCornerRoundness(win), windowOpacity),
                group.localToGlobal);
        }

        // PopupSurface entries are not windows. Compose them immediately above
        // their parent WindowGroup and before the next unrelated window.
        if (matchingSurfaceKey != 0) {
            std::vector<SurfaceRegistry::SnapshotEntry> popups;
            for (const auto& surface : surfaces) {
                const auto* popup = surface.entry;
                if (popup && popup->parentSurfaceKey == matchingSurfaceKey &&
                    popup->hasCommittedBuffer && popup->hasRenderableBuffer() &&
                    !popup->pendingDestroy) {
                    popups.push_back(surface);
                }
            }
            std::sort(popups.begin(), popups.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.entry->popupOrder < rhs.entry->popupOrder;
            });

            for (const auto& popupSurface : popups) {
                const auto* popup = popupSurface.entry;
                const auto popupBounds = resolvePopupSurfaceBounds(
                    win, *matchingSurface, *popup);
                const float opacity = windowOpacity *
                    std::clamp(popup->transitionOpacity, 0.0f, 1.0f);
                if (popup->pixels) {
                    drawShmSurface(
                        popupSurface.key, popup->shmContentSerial,
                        popupBounds.x, popupBounds.y,
                        static_cast<int>(popup->width), static_cast<int>(popup->height),
                        static_cast<int>(popup->backingWidth),
                        static_cast<int>(popup->backingHeight),
                        reinterpret_cast<const uint32_t*>(popup->pixels),
                        static_cast<int>(popup->stride / 4),
                        static_cast<int>(popup->shmDamageX),
                        static_cast<int>(popup->shmDamageY),
                        static_cast<int>(popup->shmDamageWidth),
                        static_cast<int>(popup->shmDamageHeight), opacity,
                        0.0f, 2.0f, false,
                        popupBounds.width, popupBounds.height);
                } else if (popup->rasterLayerTexture != 0) {
                    raster->drawDmaBufTextureTransformed(
                        popupBounds.x, popupBounds.y,
                        static_cast<int>(popup->width),
                        static_cast<int>(popup->height),
                        static_cast<int>(popup->backingWidth),
                        static_cast<int>(popup->backingHeight),
                        popup->rasterLayerTexture, opacity,
                        0.0f, 2.0f, false,
                        popupBounds.width, popupBounds.height);
                }

                if (popup->insetBorderEnabled) {
                    constexpr float kPopupCornerRadiusLogical = 10.0f;
                    const float borderRadius = popup->cornerRadius >= 0.0f
                        ? popup->cornerRadius
                        : kPopupCornerRadiusLogical;
                    const graphics::RectF popupLogical{
                        popup->popupX,
                        popup->popupY,
                        popup->initialWidth, popup->initialHeight};
                    replayLogicalList(
                        buildDoubleInsetBorderDisplayList(
                            popupLogical, borderRadius,
                            std::clamp(popup->cornerRoundness, 2.0f, 8.0f),
                            opacity),
                        group.localToGlobal);
                }
            }
        }

        if (!atomicConfigurePending) {
            const float retainedCornerRadius =
                matchingSurface && matchingSurface->launchMorphActive
                    ? matchingSurface->launchMorphCornerRadius
                    : group.mapLength(resolveWindowCornerRadiusLogical(win));
            const float retainedCornerRoundness =
                matchingSurface && matchingSurface->launchMorphActive
                    ? matchingSurface->launchMorphCornerRoundness
                    : resolveWindowCornerRoundness(win);
            retainWindowGroup(
                win.id, groupVisualBounds,
                retainedCornerRadius, retainedCornerRoundness);
        }
    }

    // 3. Hardware cursor only (no software cursor fallback)
    if (displayBackend.isHardwareCursorActive()) {
        displayBackend.moveHardwareCursor(
            static_cast<int>(std::lround(windowManager.getMouseX() * outputScale)),
            static_cast<int>(std::lround(windowManager.getMouseY() * outputScale)));
    }

    // Presentation and diagnostic overlays must not inherit scene damage.
    raster->setFrameDamageRect(std::nullopt);
    raster->setClipRect(std::nullopt);
    if (beforePresent) beforePresent();
    renderer.swapBuffers();

    for (const auto& surface : surfaces) {
        const auto* entry = surface.entry;
        if (!entry || entry->atomicConfigureGeneration != 0) continue;
        m_composedSurfaceFrames[surface.key] = {
            entry->frameSerial,
            entry->shmContentSerial,
            entry->rasterLayerId,
        };
    }
}

} // namespace lcl::core
