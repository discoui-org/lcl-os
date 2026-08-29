#include "core/retained_render_tree.hpp"

#include "lcl-ui/widgets/backdrop_surface.hpp"
#include "lcl-ui/widgets/filter_group.hpp"
#include "lcl-ui/widgets/scroll_view.hpp"
#include "lcl-ui/widgets/widget.hpp"

#include <bit>
#include <cmath>
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <utility>

namespace lcl::ui::detail {

struct RetainedRenderAccess {
    static uint64_t localPaintRevision(const Widget& widget) noexcept {
        return widget.m_localPaintRevision;
    }

    static uint64_t localPresentationRevision(const Widget& widget) noexcept {
        return widget.m_localPresentationRevision;
    }
};

namespace {

constexpr float kIdentityEpsilon = 1.0e-6f;
constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

void hashValue(uint64_t& hash, uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & 0xffu;
        hash *= kFnvPrime;
    }
}

void hashFloat(uint64_t& hash, float value) noexcept {
    hashValue(hash, std::bit_cast<uint32_t>(value));
}

void hashRect(uint64_t& hash, const graphics::RectF& rect) noexcept {
    hashFloat(hash, rect.x);
    hashFloat(hash, rect.y);
    hashFloat(hash, rect.width);
    hashFloat(hash, rect.height);
}

void hashPresentation(uint64_t& hash,
                      const PresentationState& state) noexcept {
    hashFloat(hash, state.opacity);
    hashFloat(hash, state.translationX);
    hashFloat(hash, state.translationY);
    hashFloat(hash, state.scaleX);
    hashFloat(hash, state.scaleY);
    hashFloat(hash, state.rotationRadians);
    hashFloat(hash, state.originX);
    hashFloat(hash, state.originY);
}

bool differs(float lhs, float rhs) noexcept {
    return std::fabs(lhs - rhs) > kIdentityEpsilon;
}

bool hasPresentationTransform(const PresentationState& state) noexcept {
    return differs(state.translationX, 0.0f) ||
           differs(state.translationY, 0.0f) ||
           differs(state.scaleX, 1.0f) ||
           differs(state.scaleY, 1.0f) ||
           differs(state.rotationRadians, 0.0f);
}

bool isEffectBoundary(const Widget& widget) noexcept {
    return dynamic_cast<const BackdropSurface*>(&widget) != nullptr ||
           dynamic_cast<const FilterGroup*>(&widget) != nullptr;
}

bool sameRect(const graphics::RectF& lhs,
              const graphics::RectF& rhs) noexcept {
    return lhs.x == rhs.x && lhs.y == rhs.y &&
           lhs.width == rhs.width && lhs.height == rhs.height;
}

bool samePresentation(const PresentationState& lhs,
                      const PresentationState& rhs) noexcept {
    return lhs.opacity == rhs.opacity &&
           lhs.translationX == rhs.translationX &&
           lhs.translationY == rhs.translationY &&
           lhs.scaleX == rhs.scaleX &&
           lhs.scaleY == rhs.scaleY &&
           lhs.rotationRadians == rhs.rotationRadians &&
           lhs.originX == rhs.originX && lhs.originY == rhs.originY;
}

bool sameOptionalRect(const std::optional<graphics::RectF>& lhs,
                      const std::optional<graphics::RectF>& rhs) noexcept {
    if (lhs.has_value() != rhs.has_value()) return false;
    return !lhs.has_value() || sameRect(*lhs, *rhs);
}

bool sameProperties(const RetainedRenderNode& lhs,
                    const RetainedRenderNode& rhs) noexcept {
    return lhs.parentId == rhs.parentId &&
           lhs.siblingIndex == rhs.siblingIndex &&
           lhs.boundaryReasons == rhs.boundaryReasons &&
           sameRect(lhs.layoutBounds, rhs.layoutBounds) &&
           sameRect(lhs.presentationBounds, rhs.presentationBounds) &&
           sameOptionalRect(lhs.clipBounds, rhs.clipBounds) &&
           samePresentation(lhs.presentation, rhs.presentation) &&
           lhs.children == rhs.children;
}

using CandidateMap = std::unordered_map<uint64_t, const RenderNode*>;

void hashOwnedContent(const RenderNode& candidate,
                      bool ownerRoot,
                      const CandidateMap& candidates,
                      uint64_t& hash,
                      std::vector<uint64_t>& retainedChildren) {
    if (!ownerRoot &&
        candidate.disposition == RenderNodeDisposition::Retained) {
        hashValue(hash, 0x52455441494e4544ull);
        hashValue(hash, candidate.id);
        retainedChildren.push_back(candidate.id);
        return;
    }

    hashValue(hash, ownerRoot ? 0x4f574e4552ull : 0x464c415454454eull);
    hashValue(hash, candidate.id);
    hashValue(hash, candidate.contentRevision);
    if (ownerRoot) {
        // Position and presentation are compositing properties of the owner.
        // Its local raster content still depends on its logical extent.
        hashFloat(hash, candidate.layoutBounds.width);
        hashFloat(hash, candidate.layoutBounds.height);
    } else {
        // Flattened descendants are baked into the owner's DisplayList.
        hashValue(hash, candidate.propertyRevision);
        hashRect(hash, candidate.layoutBounds);
        hashRect(hash, candidate.presentationBounds);
        hashPresentation(hash, candidate.presentation);
        hashValue(hash, candidate.clipBounds.has_value() ? 1u : 0u);
        if (candidate.clipBounds) hashRect(hash, *candidate.clipBounds);
    }

    hashValue(hash, candidate.children.size());
    for (uint64_t childId : candidate.children) {
        const auto child = candidates.find(childId);
        if (child == candidates.end()) continue;
        hashOwnedContent(
            *child->second, false, candidates, hash, retainedChildren);
    }
    hashValue(hash, 0x454e44ull);
}

void buildRetainedNode(const RenderNode& candidate,
                       uint64_t parentId,
                       uint32_t siblingIndex,
                       const CandidateMap& candidates,
                       std::vector<RetainedRenderNode>& output) {
    RetainedRenderNode retained;
    retained.id = candidate.id;
    retained.parentId = parentId;
    retained.siblingIndex = siblingIndex;
    retained.boundaryReasons = candidate.boundaryReasons;
    retained.layoutBounds = candidate.layoutBounds;
    retained.presentationBounds = candidate.presentationBounds;
    retained.clipBounds = candidate.clipBounds;
    retained.presentation = candidate.presentation;
    retained.propertyRevision = candidate.propertyRevision;

    uint64_t contentRevision = kFnvOffset;
    hashOwnedContent(
        candidate, true, candidates, contentRevision, retained.children);
    retained.contentRevision = contentRevision;

    const std::vector<uint64_t> retainedChildren = retained.children;
    output.push_back(std::move(retained));
    for (std::size_t index = 0; index < retainedChildren.size(); ++index) {
        const uint64_t childId = retainedChildren[index];
        const auto child = candidates.find(childId);
        if (child == candidates.end()) continue;
        buildRetainedNode(
            *child->second, candidate.id, static_cast<uint32_t>(index),
            candidates, output);
    }
}

std::vector<RetainedRenderNode> buildRetainedNodes(const RenderTree& tree) {
    CandidateMap candidates;
    candidates.reserve(tree.nodes.size());
    for (const auto& node : tree.nodes) {
        candidates.emplace(node.id, &node);
    }

    std::vector<RetainedRenderNode> output;
    output.reserve(tree.retainedNodeCount());
    const auto root = candidates.find(tree.rootId);
    if (root != candidates.end() &&
        root->second->disposition == RenderNodeDisposition::Retained) {
        buildRetainedNode(*root->second, 0, 0, candidates, output);
    }
    return output;
}

std::optional<std::size_t> compileWidget(const Widget& widget,
                                         std::optional<std::size_t> parentIndex,
                                         const ScrollView* parentScrollView,
                                         bool isRoot,
                                         RenderTree& tree) {
    if (!widget.isVisible()) return std::nullopt;

    const auto& presentation = widget.getPresentationState();
    const auto* scrollView = dynamic_cast<const ScrollView*>(&widget);
    const bool isScrollContent = parentScrollView != nullptr &&
                                 parentScrollView->getContent() == &widget;

    RenderBoundaryReason reasons = RenderBoundaryReason::None;
    if (isRoot) reasons |= RenderBoundaryReason::Root;
    if (widget.clipsToBounds()) reasons |= RenderBoundaryReason::Clip;
    if (hasPresentationTransform(presentation)) {
        reasons |= RenderBoundaryReason::Transform;
    }
    if (differs(presentation.opacity, 1.0f)) {
        reasons |= RenderBoundaryReason::Opacity;
    }
    if (scrollView != nullptr) reasons |= RenderBoundaryReason::ScrollViewport;
    if (isScrollContent) reasons |= RenderBoundaryReason::ScrollContent;
    if (isEffectBoundary(widget)) reasons |= RenderBoundaryReason::Effect;

    RenderNode node;
    node.id = widget.getObjectId();
    node.parentId = parentIndex.has_value() ? tree.nodes[*parentIndex].id : 0;
    node.disposition = reasons == RenderBoundaryReason::None
        ? RenderNodeDisposition::Flattened
        : RenderNodeDisposition::Retained;
    node.boundaryReasons = reasons;
    node.layoutBounds = widget.getAbsoluteBounds();
    node.presentationBounds = widget.getPresentationBounds();
    if (widget.clipsToBounds()) {
        node.clipBounds = node.presentationBounds;
    }
    node.presentation = presentation;
    node.contentRevision = RetainedRenderAccess::localPaintRevision(widget);
    node.propertyRevision =
        RetainedRenderAccess::localPresentationRevision(widget);
    node.layoutDirty = widget.isLayoutDirty();

    const std::size_t nodeIndex = tree.nodes.size();
    tree.nodes.push_back(std::move(node));
    if (isRoot) tree.rootId = widget.getObjectId();

    for (const auto& child : widget.getChildren()) {
        if (!child) continue;
        const auto childIndex = compileWidget(
            *child, nodeIndex, scrollView, false, tree);
        if (childIndex.has_value()) {
            tree.nodes[nodeIndex].children.push_back(
                tree.nodes[*childIndex].id);
        }
    }

    return nodeIndex;
}

} // namespace

const RenderNode* RenderTree::find(uint64_t id) const noexcept {
    for (const auto& node : nodes) {
        if (node.id == id) return &node;
    }
    return nullptr;
}

std::size_t RenderTree::retainedNodeCount() const noexcept {
    std::size_t count = 0;
    for (const auto& node : nodes) {
        if (node.disposition == RenderNodeDisposition::Retained) ++count;
    }
    return count;
}

RenderTree RenderNodeCompiler::compile(const Widget& root,
                                       std::size_t reserveHint) const {
    RenderTree tree;
    tree.nodes.reserve(reserveHint);
    compileWidget(root, std::nullopt, nullptr, true, tree);
    tree.retainedNodes = buildRetainedNodes(tree);
    return tree;
}

RenderTreeTransaction RenderTreeDiffer::diff(
        const RenderTree* previous,
        const RenderTree& current,
        bool forceReplace) const {
    RenderTreeTransaction transaction;
    const auto& currentNodes = current.retainedNodes;
    if (forceReplace || previous == nullptr) {
        transaction.replacesTree = true;
        transaction.creates = currentNodes;
        return transaction;
    }

    const auto& previousNodes = previous->retainedNodes;
    std::unordered_map<uint64_t, const RetainedRenderNode*> previousById;
    std::unordered_map<uint64_t, const RetainedRenderNode*> currentById;
    previousById.reserve(previousNodes.size());
    currentById.reserve(currentNodes.size());
    for (const auto& node : previousNodes) previousById.emplace(node.id, &node);
    for (const auto& node : currentNodes) currentById.emplace(node.id, &node);

    // Creates and updates follow current pre-order, so parents exist before
    // children and sibling ordering is deterministic.
    for (const auto& node : currentNodes) {
        const auto old = previousById.find(node.id);
        if (old == previousById.end()) {
            transaction.creates.push_back(node);
            continue;
        }

        const bool contentChanged =
            node.contentRevision != old->second->contentRevision;
        const bool propertiesChanged = !sameProperties(node, *old->second);
        if (contentChanged || propertiesChanged) {
            transaction.updates.push_back({
                node, contentChanged, propertiesChanged});
        }
    }

    // Reverse previous pre-order removes descendants before their parents.
    for (auto node = previousNodes.rbegin(); node != previousNodes.rend(); ++node) {
        if (!currentById.contains(node->id)) {
            transaction.removals.push_back(node->id);
        }
    }
    return transaction;
}

} // namespace lcl::ui::detail
