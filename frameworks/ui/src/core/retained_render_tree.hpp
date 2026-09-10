#pragma once

#include "lcl-graphics/geometry.hpp"
#include "lcl-ui/core/motion.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace lcl::ui {

class Widget;

namespace detail {

enum class RenderBoundaryReason : uint32_t {
    None = 0,
    Root = 1u << 0u,
    Clip = 1u << 1u,
    Transform = 1u << 2u,
    Opacity = 1u << 3u,
    ScrollViewport = 1u << 4u,
    ScrollContent = 1u << 5u,
    Effect = 1u << 6u,
    ExternalBuffer = 1u << 7u,
    RetainedPresentation = 1u << 8u,
};

constexpr RenderBoundaryReason operator|(RenderBoundaryReason lhs,
                                         RenderBoundaryReason rhs) noexcept {
    return static_cast<RenderBoundaryReason>(
        static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

constexpr RenderBoundaryReason& operator|=(RenderBoundaryReason& lhs,
                                           RenderBoundaryReason rhs) noexcept {
    lhs = lhs | rhs;
    return lhs;
}

constexpr bool hasBoundaryReason(RenderBoundaryReason reasons,
                                 RenderBoundaryReason reason) noexcept {
    return (static_cast<uint32_t>(reasons) & static_cast<uint32_t>(reason)) != 0;
}

enum class RenderNodeDisposition : uint8_t {
    Flattened,
    Retained,
};

/**
 * Backend-neutral value snapshot of one Widget's render semantics.
 *
 * A node is present for every visible Widget so ordering and parentage remain
 * deterministic. Only Retained nodes are candidates for future independent
 * raster/compositing storage; Flattened nodes stay in their nearest retained
 * ancestor's display list.
 */
struct RenderNode {
    uint64_t id{0};
    uint64_t parentId{0};
    RenderNodeDisposition disposition{RenderNodeDisposition::Flattened};
    RenderBoundaryReason boundaryReasons{RenderBoundaryReason::None};
    graphics::RectF layoutBounds{};
    graphics::RectF presentationBounds{};
    std::optional<graphics::RectF> clipBounds{};
    PresentationState presentation{};
    uint64_t contentRevision{0};
    uint64_t propertyRevision{0};
    uint64_t externalBufferId{0};
    uint64_t externalBufferRevision{0};
    bool layoutDirty{false};
    std::vector<uint64_t> children{};
};

/** One actual retained/compositing node after flattened ownership is resolved. */
struct RetainedRenderNode {
    uint64_t id{0};
    uint64_t parentId{0};
    uint32_t siblingIndex{0};
    RenderBoundaryReason boundaryReasons{RenderBoundaryReason::None};
    graphics::RectF layoutBounds{};
    graphics::RectF presentationBounds{};
    std::optional<graphics::RectF> clipBounds{};
    PresentationState presentation{};
    // Fingerprint of content owned by this boundary, including flattened
    // descendants but excluding nested retained boundaries.
    uint64_t contentRevision{0};
    uint64_t propertyRevision{0};
    uint64_t externalBufferId{0};
    uint64_t externalBufferRevision{0};
    std::vector<uint64_t> children{};
};

struct RetainedRenderNodeUpdate {
    RetainedRenderNode node{};
    bool contentChanged{false};
    bool propertiesChanged{false};
};

struct RenderTreeTransaction {
    bool replacesTree{false};
    std::vector<RetainedRenderNode> creates{};
    std::vector<RetainedRenderNodeUpdate> updates{};
    // Child-before-parent order makes removal valid without implicit cascade.
    std::vector<uint64_t> removals{};

    bool empty() const noexcept {
        return !replacesTree && creates.empty() && updates.empty() &&
               removals.empty();
    }
};

struct RenderTree {
    uint64_t rootId{0};
    std::vector<RenderNode> nodes{};
    std::vector<RetainedRenderNode> retainedNodes{};

    const RenderNode* find(uint64_t id) const noexcept;
    std::size_t retainedNodeCount() const noexcept;
};

/** Compiles a Widget tree into stable pre-order render semantics. */
class RenderNodeCompiler final {
public:
    RenderTree compile(const Widget& root,
                       std::size_t reserveHint = 0) const;
};

/** Produces stable retained-node mutations between two compiled snapshots. */
class RenderTreeDiffer final {
public:
    RenderTreeTransaction diff(const RenderTree* previous,
                               const RenderTree& current,
                               bool forceReplace = false) const;
};

} // namespace detail
} // namespace lcl::ui
