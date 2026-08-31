#pragma once

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>

#include "system/ipc/lcl_protocol.hpp"

namespace lcl::core {

struct LocalEffectGeometry {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
};

inline bool filterReadsNeighboringPixels(protocol::FilterType type) noexcept {
    return type == protocol::FilterType::Blur ||
           type == protocol::FilterType::Glass;
}

inline bool shouldExecuteEffectSource(
        protocol::EffectSourceType source,
        bool hardwareAccelerated) noexcept {
    return source != protocol::EffectSourceType::SurfaceBackdrop ||
           hardwareAccelerated;
}

inline bool canReuseRetainedBackdropForClientDamage(
        size_t changedSurfaceCount,
        size_t nonLocalEffectSurfaceCount,
        bool nonLocalEffectsAreBackdropOnly,
        bool changedSurfaceOwnsNonLocalEffect,
        bool retainedBackdropMatches) noexcept {
    return changedSurfaceCount == 1 &&
           nonLocalEffectSurfaceCount == 1 &&
           nonLocalEffectsAreBackdropOnly &&
           changedSurfaceOwnsNonLocalEffect &&
           retainedBackdropMatches;
}

inline bool effectMatchesLogicalSurfaceBounds(
    const protocol::EffectRegion& region,
    float logicalWidth,
    float logicalHeight) noexcept {
    constexpr float kGeometryEpsilon = 0.001f;
    return std::fabs(region.x) <= kGeometryEpsilon &&
           std::fabs(region.y) <= kGeometryEpsilon &&
           std::fabs(region.width - logicalWidth) <= kGeometryEpsilon &&
           std::fabs(region.height - logicalHeight) <= kGeometryEpsilon;
}

inline LocalEffectGeometry resolveLocalEffectGeometry(
    float windowX,
    float windowY,
    float titleOffset,
    float surfaceWidth,
    float surfaceHeight,
    const protocol::EffectRegion& region,
    bool followSurfaceBounds) {
    return {
        windowX + (followSurfaceBounds ? 0.0f : region.x),
        windowY + titleOffset + (followSurfaceBounds ? 0.0f : region.y),
        followSurfaceBounds ? surfaceWidth : region.width,
        followSurfaceBounds ? surfaceHeight : region.height,
    };
}

} // namespace lcl::core
