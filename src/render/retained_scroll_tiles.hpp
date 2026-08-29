#pragma once

#include "core/ipc/raster_protocol.hpp"
#include "lcl-graphics/display_list.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace lcl::render {

/**
 * Rasterd-owned retained composition cache.
 *
 * ScrollContent bodies are split into visible and one-tile-nearby textures.
 * Small non-scroll presentation boundaries retain one logical cached layer so
 * transform/opacity-only commits can recompose it without another DisplayList
 * upload from the application.
 */
class RetainedScrollTileCache final {
public:
    static constexpr float kLogicalTileHeight = 384.0f;

    using NodeMap = std::unordered_map<
        uint64_t, raster_protocol::RetainedNodeState>;
    using LayerNamespaceMap = std::unordered_map<uint64_t, uint64_t>;
    using AllocateLayerId = std::function<uint64_t()>;

    struct Patch {
        uint64_t serial{0};
        graphics::RectF bounds{};
        std::shared_ptr<const std::vector<graphics::DisplayCommand>> commands{};
    };

    struct ResidentTile {
        uint64_t layerId{0};
        uint64_t appliedPatchSerial{0};
    };

    struct ContentCache {
        uint64_t contentNodeId{0};
        uint64_t viewportNodeId{0};
        uint64_t sourceLayerId{0};
        uint64_t contentRevision{0};
        uint64_t nextPatchSerial{1};
        graphics::RectF sourceBounds{};
        std::shared_ptr<const std::vector<graphics::DisplayCommand>>
            baseCommands{};
        std::vector<Patch> patches{};
        std::unordered_map<int64_t, ResidentTile> residentTiles{};
    };

    struct PreparedFrame {
        std::unique_ptr<RetainedScrollTileCache> next{};
        graphics::DisplayList displayList{};
        std::vector<uint64_t> createdLayerIds{};
        std::vector<uint64_t> evictedLayerIds{};
        bool tiled{false};
        bool retainedComposition{false};
    };

    /**
     * Prepares a transactional retained replay. `submittedCommands ==
     * nullptr` denotes a property-only scroll or presentation commit.
     */
    static std::optional<PreparedFrame> prepare(
        const RetainedScrollTileCache& current,
        const std::vector<graphics::DisplayCommand>* submittedCommands,
        const NodeMap& nextNodes,
        const LayerNamespaceMap& layerNamespaces,
        const AllocateLayerId& allocateLayerId,
        bool replaceScene = false,
        bool refreshCompositionTemplate = false,
        bool invalidateCompositionTemplate = false) {
        PreparedFrame prepared;
        if (replaceScene) {
            prepared.evictedLayerIds = current.releaseAll();
        }
        prepared.next = std::make_unique<RetainedScrollTileCache>(
            replaceScene ? RetainedScrollTileCache{} : current);

        const auto contentByLayer = scrollContentByLayer(
            nextNodes, layerNamespaces);
        const auto presentationByLayer = retainedPresentationByLayer(
            nextNodes, layerNamespaces);
        prepared.next->removeDeadContent(nextNodes, prepared.evictedLayerIds);

        std::vector<graphics::DisplayCommand> genericLayerUpdates;
        if (submittedCommands) {
            auto composition = prepared.next->ingest(
                *submittedCommands, nextNodes, contentByLayer,
                presentationByLayer, genericLayerUpdates,
                prepared.evictedLayerIds);
            if (composition &&
                (replaceScene || refreshCompositionTemplate ||
                 invalidateCompositionTemplate)) {
                // Geometry/outer-scene changes invalidate the previous
                // composition, but the current damage frame may already carry
                // a complete ScrollView draw for the affected viewport. Adopt
                // that current template immediately instead of rasterizing a
                // temporary content-sized source layer for one frame.
                prepared.next->m_compositionTemplate = std::move(*composition);
            } else if (invalidateCompositionTemplate) {
                // A frame damaged outside every ScrollView may legitimately
                // contain no scroll draw. Keep content/tile state, but require
                // one ordinary frame before the next property-only fast path.
                prepared.next->m_compositionTemplate.reset();
            }
        }

        if (!prepared.next->m_compositionTemplate) {
            if (!submittedCommands) return std::nullopt;
            const auto passthrough = std::make_shared<std::vector<
                graphics::DisplayCommand>>(*submittedCommands);
            const auto immutable = std::shared_ptr<
                const std::vector<graphics::DisplayCommand>>(passthrough);
            prepared.displayList = graphics::DisplayList(immutable);
            deduplicate(prepared.evictedLayerIds);
            return prepared;
        }
        auto commands = std::make_shared<std::vector<
            graphics::DisplayCommand>>();
        commands->reserve(
            genericLayerUpdates.size() +
            prepared.next->m_compositionTemplate->commands().size() + 24);
        commands->insert(
            commands->end(), genericLayerUpdates.begin(),
            genericLayerUpdates.end());

        for (const auto& command :
             prepared.next->m_compositionTemplate->commands()) {
            const auto* draw = std::get_if<
                graphics::DrawCachedLayerCommand>(&command);
            if (!draw) {
                commands->push_back(command);
                continue;
            }
            auto cache = prepared.next->findBySourceLayer(draw->id);
            if (cache == prepared.next->m_content.end()) {
                const auto presentation = presentationByLayer.find(draw->id);
                if (presentation != presentationByLayer.end()) {
                    const auto node = nextNodes.find(presentation->second);
                    if (node == nextNodes.end()) return std::nullopt;
                    auto patched = *draw;
                    patched.transform = presentationMatrix(node->second);
                    patched.opacity = node->second.opacity;
                    commands->emplace_back(std::move(patched));
                    prepared.retainedComposition = true;
                    continue;
                }
                commands->push_back(command);
                continue;
            }
            const auto node = nextNodes.find(cache->second.contentNodeId);
            const auto viewport = nextNodes.find(cache->second.viewportNodeId);
            if (node == nextNodes.end() || viewport == nextNodes.end() ||
                !cache->second.baseCommands ||
                cache->second.baseCommands->empty() ||
                cache->second.sourceBounds.isEmpty()) {
                return std::nullopt;
            }
            prepared.next->appendTiles(
                cache->second, node->second, viewport->second,
                allocateLayerId, *commands, prepared.createdLayerIds,
                prepared.evictedLayerIds);
            prepared.tiled = true;
            prepared.retainedComposition = true;
        }
        if (!prepared.retainedComposition) {
            if (!submittedCommands) return std::nullopt;
            const auto passthrough = std::make_shared<std::vector<
                graphics::DisplayCommand>>(*submittedCommands);
            const auto immutable = std::shared_ptr<
                const std::vector<graphics::DisplayCommand>>(passthrough);
            prepared.displayList = graphics::DisplayList(immutable);
            deduplicate(prepared.evictedLayerIds);
            return prepared;
        }

        const auto immutable = std::shared_ptr<
            const std::vector<graphics::DisplayCommand>>(commands);
        prepared.displayList = graphics::DisplayList(immutable);
        deduplicate(prepared.createdLayerIds);
        deduplicate(prepared.evictedLayerIds);
        return prepared;
    }

    std::vector<uint64_t> releaseAll() const {
        std::vector<uint64_t> result;
        for (const auto& [_, cache] : m_content) {
            for (const auto& [tileIndex, tile] : cache.residentTiles) {
                (void)tileIndex;
                if (tile.layerId != 0) result.push_back(tile.layerId);
            }
        }
        deduplicate(result);
        return result;
    }

    std::size_t contentCount() const noexcept { return m_content.size(); }

    std::size_t residentTileCount() const noexcept {
        std::size_t result = 0;
        for (const auto& [_, cache] : m_content) {
            result += cache.residentTiles.size();
        }
        return result;
    }

private:
    using ContentMap = std::unordered_map<uint64_t, ContentCache>;

    static constexpr uint32_t kScrollViewportReason = 1u << 4u;
    static constexpr uint32_t kScrollContentReason = 1u << 5u;
    static constexpr uint32_t kRootReason = 1u << 0u;
    static constexpr uint32_t kTransformReason = 1u << 2u;
    static constexpr uint32_t kOpacityReason = 1u << 3u;
    static constexpr float kEpsilon = 0.001f;

    static bool isScrollViewport(
            const raster_protocol::RetainedNodeState& node) noexcept {
        return (node.boundaryReasons & kScrollViewportReason) != 0;
    }

    static bool isScrollContent(
            const raster_protocol::RetainedNodeState& node) noexcept {
        return (node.boundaryReasons & kScrollContentReason) != 0;
    }

    static bool isRetainedPresentationCandidate(
            const raster_protocol::RetainedNodeState& node) noexcept {
        const bool presentation =
            (node.boundaryReasons &
             (kTransformReason | kOpacityReason)) != 0;
        const float area = node.layoutWidth * node.layoutHeight;
        return presentation &&
            (node.boundaryReasons & kRootReason) == 0 &&
            !isScrollViewport(node) && !isScrollContent(node) &&
            node.layoutWidth > 0.0f && node.layoutHeight > 0.0f &&
            node.layoutWidth <= 2048.0f && node.layoutHeight <= 2048.0f &&
            area <= 4194304.0f;
    }

    static bool contains(const graphics::RectF& outer,
                         const graphics::RectF& inner) noexcept {
        return outer.x <= inner.x + kEpsilon &&
            outer.y <= inner.y + kEpsilon &&
            outer.x + outer.width + kEpsilon >= inner.x + inner.width &&
            outer.y + outer.height + kEpsilon >= inner.y + inner.height;
    }

    static void deduplicate(std::vector<uint64_t>& ids) {
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    }

    static std::unordered_map<uint64_t, uint64_t> scrollContentByLayer(
            const NodeMap& nodes,
            const LayerNamespaceMap& layerNamespaces) {
        std::unordered_map<uint64_t, uint64_t> result;
        for (const auto& [id, node] : nodes) {
            if (!isScrollContent(node) || node.parentId == 0) continue;
            const auto viewport = nodes.find(node.parentId);
            if (viewport == nodes.end() ||
                !isScrollViewport(viewport->second)) continue;
            const auto layer = layerNamespaces.find(node.parentId);
            if (layer != layerNamespaces.end()) {
                result.emplace(layer->second, id);
            }
        }
        return result;
    }

    static bool descendsFrom(const NodeMap& nodes, uint64_t id,
                             uint64_t ancestorId) noexcept {
        auto node = nodes.find(id);
        std::size_t depth = 0;
        while (node != nodes.end() && depth++ <= nodes.size()) {
            if (node->second.parentId == ancestorId) return true;
            if (node->second.parentId == 0) return false;
            node = nodes.find(node->second.parentId);
        }
        return false;
    }

    static bool isWithinScrollContent(const NodeMap& nodes,
                                      uint64_t id) noexcept {
        auto node = nodes.find(id);
        std::size_t depth = 0;
        while (node != nodes.end() && depth++ <= nodes.size()) {
            if (isScrollContent(node->second)) return true;
            if (node->second.parentId == 0) return false;
            node = nodes.find(node->second.parentId);
        }
        return false;
    }

    static std::unordered_set<uint64_t> retainedPresentationNodes(
            const NodeMap& nodes) {
        std::unordered_set<uint64_t> result;
        for (const auto& [id, node] : nodes) {
            if (!isRetainedPresentationCandidate(node) ||
                isWithinScrollContent(nodes, id)) {
                continue;
            }
            bool hasCandidateAncestor = false;
            for (const auto& [ancestorId, ancestor] : nodes) {
                if (ancestorId != id &&
                    isRetainedPresentationCandidate(ancestor) &&
                    descendsFrom(nodes, id, ancestorId)) {
                    hasCandidateAncestor = true;
                    break;
                }
            }
            if (hasCandidateAncestor) continue;
            const bool containsScroll = std::any_of(
                nodes.begin(), nodes.end(), [&](const auto& descendant) {
                    return (isScrollViewport(descendant.second) ||
                            isScrollContent(descendant.second)) &&
                        descendsFrom(nodes, descendant.first, id);
                });
            if (!containsScroll) result.insert(id);
        }
        return result;
    }

    static std::unordered_map<uint64_t, uint64_t>
    retainedPresentationByLayer(
            const NodeMap& nodes,
            const LayerNamespaceMap& layerNamespaces) {
        std::unordered_map<uint64_t, uint64_t> result;
        for (const uint64_t id : retainedPresentationNodes(nodes)) {
            const auto layer = layerNamespaces.find(id);
            if (layer != layerNamespaces.end()) {
                result.emplace(layer->second, id);
            }
        }
        return result;
    }

    static graphics::Matrix3 presentationMatrix(
            const raster_protocol::RetainedNodeState& node) noexcept {
        const float ox = node.layoutX + node.layoutWidth * node.originX;
        const float oy = node.layoutY + node.layoutHeight * node.originY;
        const float cosine = std::cos(node.rotationRadians);
        const float sine = std::sin(node.rotationRadians);
        return {
            cosine * node.scaleX,
            sine * node.scaleX,
            -sine * node.scaleY,
            cosine * node.scaleY,
            ox + node.translationX - cosine * node.scaleX * ox +
                sine * node.scaleY * oy,
            oy + node.translationY - sine * node.scaleX * ox -
                cosine * node.scaleY * oy,
        };
    }

    void removeDeadContent(const NodeMap& nodes,
                           std::vector<uint64_t>& evicted) {
        for (auto cache = m_content.begin(); cache != m_content.end();) {
            const auto node = nodes.find(cache->first);
            if (node != nodes.end() && isScrollContent(node->second) &&
                node->second.parentId == cache->second.viewportNodeId) {
                ++cache;
                continue;
            }
            for (const auto& [_, tile] : cache->second.residentTiles) {
                if (tile.layerId != 0) evicted.push_back(tile.layerId);
            }
            cache = m_content.erase(cache);
        }
    }

    std::optional<graphics::DisplayList> ingest(
            const std::vector<graphics::DisplayCommand>& submitted,
            const NodeMap& nodes,
            const std::unordered_map<uint64_t, uint64_t>& contentByLayer,
            const std::unordered_map<uint64_t, uint64_t>&
                presentationByLayer,
            std::vector<graphics::DisplayCommand>& genericLayerUpdates,
            std::vector<uint64_t>& evicted) {
        auto composition = std::make_shared<std::vector<
            graphics::DisplayCommand>>();
        composition->reserve(submitted.size());
        bool hasRetainedDraw = false;

        for (std::size_t index = 0; index < submitted.size();) {
            const auto* begin = std::get_if<
                graphics::BeginCachedLayerCommand>(&submitted[index]);
            const auto content = begin
                ? contentByLayer.find(begin->id)
                : contentByLayer.end();
            const bool genericBegin = begin &&
                presentationByLayer.contains(begin->id);
            if (!begin ||
                (content == contentByLayer.end() && !genericBegin)) {
                if (const auto* draw = std::get_if<
                        graphics::DrawCachedLayerCommand>(
                            &submitted[index]);
                    draw && (contentByLayer.contains(draw->id) ||
                             presentationByLayer.contains(draw->id))) {
                    hasRetainedDraw = true;
                }
                composition->push_back(submitted[index]);
                ++index;
                continue;
            }

            std::size_t end = index + 1;
            std::size_t depth = 1;
            for (; end < submitted.size() && depth != 0; ++end) {
                if (std::holds_alternative<
                        graphics::BeginCachedLayerCommand>(submitted[end])) {
                    ++depth;
                } else if (std::holds_alternative<
                               graphics::EndCachedLayerCommand>(
                                   submitted[end])) {
                    --depth;
                }
            }
            if (depth != 0 || end <= index + 1) return std::nullopt;
            if (genericBegin) {
                genericLayerUpdates.insert(
                    genericLayerUpdates.end(),
                    submitted.begin() + static_cast<std::ptrdiff_t>(index),
                    submitted.begin() + static_cast<std::ptrdiff_t>(end));
                index = end;
                continue;
            }
            const auto node = nodes.find(content->second);
            if (node == nodes.end() || node->second.parentId == 0) {
                return std::nullopt;
            }

            auto& cache = m_content[content->second];
            cache.contentNodeId = content->second;
            cache.viewportNodeId = node->second.parentId;
            cache.sourceLayerId = begin->id;
            cache.contentRevision = node->second.contentRevision;
            std::vector<graphics::DisplayCommand> body(
                submitted.begin() + static_cast<std::ptrdiff_t>(index + 1),
                submitted.begin() + static_cast<std::ptrdiff_t>(end - 1));

            if (!begin->updateBounds) {
                for (const auto& [_, tile] : cache.residentTiles) {
                    if (tile.layerId != 0) evicted.push_back(tile.layerId);
                }
                cache.sourceBounds = begin->sourceBounds;
                cache.baseCommands = std::make_shared<const std::vector<
                    graphics::DisplayCommand>>(std::move(body));
                cache.patches.clear();
                cache.residentTiles.clear();
                cache.nextPatchSerial = 1;
            } else {
                if (!cache.baseCommands || cache.baseCommands->empty() ||
                    !contains(cache.sourceBounds, begin->sourceBounds) ||
                    !contains(begin->sourceBounds, cache.sourceBounds)) {
                    return std::nullopt;
                }
                Patch patch{
                    cache.nextPatchSerial++, *begin->updateBounds,
                    std::make_shared<const std::vector<
                        graphics::DisplayCommand>>(std::move(body))};
                // A newer complete redraw of the same spatial region makes
                // older patches fully redundant. Spinner/button animations
                // therefore remain bounded instead of growing a frame log.
                std::erase_if(cache.patches, [&](const Patch& old) {
                    return contains(patch.bounds, old.bounds);
                });
                cache.patches.push_back(std::move(patch));
            }
            index = end;
        }

        if (!hasRetainedDraw) return std::nullopt;
        const auto immutable = std::shared_ptr<
            const std::vector<graphics::DisplayCommand>>(composition);
        return graphics::DisplayList(immutable);
    }

    ContentMap::iterator findBySourceLayer(uint64_t id) {
        return std::find_if(
            m_content.begin(), m_content.end(), [id](const auto& item) {
                return item.second.sourceLayerId == id;
            });
    }

    static graphics::RectF viewportBounds(
            const raster_protocol::RetainedNodeState& viewport) noexcept {
        if ((viewport.flags & raster_protocol::kNodeHasClip) != 0 &&
            viewport.clipWidth > 0.0f && viewport.clipHeight > 0.0f) {
            return {viewport.clipX, viewport.clipY,
                    viewport.clipWidth, viewport.clipHeight};
        }
        return {viewport.presentationX, viewport.presentationY,
                viewport.presentationWidth, viewport.presentationHeight};
    }

    static std::pair<int64_t, int64_t> desiredTileRange(
            const ContentCache& cache,
            const raster_protocol::RetainedNodeState& content,
            const raster_protocol::RetainedNodeState& viewport) noexcept {
        const graphics::RectF visible = viewportBounds(viewport);
        const float tileHeight = kLogicalTileHeight;
        const float contentTop = cache.sourceBounds.y + content.translationY;
        const float expandedTop = visible.y - tileHeight;
        const float expandedBottom = visible.y + visible.height + tileHeight;
        const int64_t tileCount = std::max<int64_t>(
            1, static_cast<int64_t>(std::ceil(
                   cache.sourceBounds.height / tileHeight)));
        const int64_t first = std::clamp<int64_t>(
            static_cast<int64_t>(std::floor(
                (expandedTop - contentTop) / tileHeight)),
            0, tileCount - 1);
        const int64_t last = std::clamp<int64_t>(
            static_cast<int64_t>(std::floor(
                (expandedBottom - contentTop - kEpsilon) / tileHeight)),
            first, tileCount - 1);
        return {first, last};
    }

    static graphics::RectF tileBounds(const ContentCache& cache,
                                      int64_t index) noexcept {
        const float y = cache.sourceBounds.y +
            static_cast<float>(index) * kLogicalTileHeight;
        const float remaining = cache.sourceBounds.y +
            cache.sourceBounds.height - y;
        return {cache.sourceBounds.x, y, cache.sourceBounds.width,
                std::max(0.0f, std::min(kLogicalTileHeight, remaining))};
    }

    static void appendLayerUpdate(
            uint64_t layerId, const graphics::RectF& tile,
            const Patch& patch,
            std::vector<graphics::DisplayCommand>& output) {
        const graphics::RectF update = tile.intersection(patch.bounds);
        if (update.isEmpty()) return;
        output.emplace_back(graphics::BeginCachedLayerCommand{
            layerId, tile, update});
        if (patch.commands) {
            output.insert(output.end(), patch.commands->begin(),
                          patch.commands->end());
        }
        output.emplace_back(graphics::EndCachedLayerCommand{});
    }

    void appendTiles(
            ContentCache& cache,
            const raster_protocol::RetainedNodeState& content,
            const raster_protocol::RetainedNodeState& viewport,
            const AllocateLayerId& allocateLayerId,
            std::vector<graphics::DisplayCommand>& output,
            std::vector<uint64_t>& created,
            std::vector<uint64_t>& evicted) {
        const auto [first, last] = desiredTileRange(cache, content, viewport);
        std::unordered_set<int64_t> desired;
        desired.reserve(static_cast<std::size_t>(last - first + 1));
        for (int64_t index = first; index <= last; ++index) {
            desired.insert(index);
        }
        for (auto tile = cache.residentTiles.begin();
             tile != cache.residentTiles.end();) {
            if (desired.contains(tile->first)) {
                ++tile;
                continue;
            }
            if (tile->second.layerId != 0) {
                evicted.push_back(tile->second.layerId);
            }
            tile = cache.residentTiles.erase(tile);
        }

        const uint64_t latestPatch = cache.patches.empty()
            ? 0 : cache.patches.back().serial;
        for (int64_t index = first; index <= last; ++index) {
            const graphics::RectF tile = tileBounds(cache, index);
            if (tile.isEmpty()) continue;
            auto resident = cache.residentTiles.find(index);
            if (resident == cache.residentTiles.end()) {
                const uint64_t layerId = allocateLayerId();
                if (layerId == 0) continue;
                ResidentTile state{layerId, 0};
                output.emplace_back(graphics::BeginCachedLayerCommand{
                    layerId, tile, std::nullopt});
                output.insert(output.end(), cache.baseCommands->begin(),
                              cache.baseCommands->end());
                output.emplace_back(graphics::EndCachedLayerCommand{});
                for (const Patch& patch : cache.patches) {
                    appendLayerUpdate(layerId, tile, patch, output);
                }
                state.appliedPatchSerial = latestPatch;
                resident = cache.residentTiles.emplace(index, state).first;
                created.push_back(layerId);
            } else if (resident->second.appliedPatchSerial < latestPatch) {
                for (const Patch& patch : cache.patches) {
                    if (patch.serial > resident->second.appliedPatchSerial) {
                        appendLayerUpdate(
                            resident->second.layerId, tile, patch, output);
                    }
                }
                resident->second.appliedPatchSerial = latestPatch;
            }

            output.emplace_back(graphics::DrawCachedLayerCommand{
                resident->second.layerId,
                {tile.x + content.translationX,
                 tile.y + content.translationY,
                 tile.width, tile.height},
                content.opacity});
        }
    }

    ContentMap m_content{};
    std::optional<graphics::DisplayList> m_compositionTemplate{};
};

} // namespace lcl::render
