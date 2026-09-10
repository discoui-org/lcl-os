#include "lcl-ui/core/window_app.hpp"
#include "core/retained_render_tree.hpp"
#include "raster_service_client.hpp"
#include "system/ipc/lcl_protocol.hpp"
#include "lcl-graphics/display_list_wire.hpp"
#include "lcl-ui/widgets/scroll_view.hpp"
#include "lcl-ui/widgets/external_buffer.hpp"
#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <cstring>
#include <signal.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <filesystem>
#include <unordered_set>

namespace lcl::ui {

static std::atomic<bool> g_appSignalReceived{false};

static void setupAppSignalHandlers() {
    static bool handlersSet = false;
    if (handlersSet) return;
    handlersSet = true;

    struct sigaction sa{};
    sa.sa_handler = [](int sig) {
        (void)sig;
        g_appSignalReceived = true;
    };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
}

namespace {

// Presentation-only page/window transforms are rasterized once and then
// composed from retained layers. Widget invalidation contributes both the old
// and new presentation bounds, while a tree/content mutation invalidates the
// composition template and forces a full recovery frame.
constexpr bool kRetainedPresentationCacheEnabled = true;

float sanitizeBufferScale(float scale) {
    if (!std::isfinite(scale) || scale < 0.5f || scale > 4.0f) {
        return 1.0f;
    }
    return scale;
}

uint32_t toBufferPixels(float logical, float scale) {
    return std::max(1u, static_cast<uint32_t>(std::ceil(logical * scale)));
}

uint64_t monotonicNowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool frameTraceEnabled() {
    const char* value = std::getenv("LCL_TRACE_FRAMES");
    return value && value[0] != '\0' && value[0] != '0';
}

bool layoutOverlayEnabled() {
    const char* value = std::getenv("LCL_DEBUG_LAYOUT");
    return value && value[0] != '\0' && value[0] != '0';
}

bool readEnvironmentFloat(const char* name, float& value) {
    const char* text = std::getenv(name);
    if (!text || text[0] == '\0') return false;
    char* end = nullptr;
    const float parsed = std::strtof(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(parsed)) return false;
    value = parsed;
    return true;
}

bool readEnvironmentUint64(const char* name, uint64_t& value) {
    const char* text = std::getenv(name);
    if (!text || text[0] == '\0') return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0) return false;
    value = static_cast<uint64_t>(parsed);
    return true;
}

void takeLaunchOrigin(lcl::protocol::LCLMsgSurfaceCreate& surface) {
    readEnvironmentUint64("LCL_LAUNCH_TOKEN", surface.launchToken);
    readEnvironmentUint64("LCL_APP_INSTANCE_ID", surface.appInstanceId);
    unsetenv("LCL_LAUNCH_TOKEN");
    unsetenv("LCL_APP_INSTANCE_ID");

    constexpr const char* kNames[] = {
        "LCL_LAUNCH_ORIGIN_X",
        "LCL_LAUNCH_ORIGIN_Y",
        "LCL_LAUNCH_ORIGIN_WIDTH",
        "LCL_LAUNCH_ORIGIN_HEIGHT",
        "LCL_LAUNCH_ORIGIN_RADIUS",
    };
    float values[5]{};
    bool valid = true;
    for (size_t index = 0; index < 5; ++index) {
        valid = readEnvironmentFloat(kNames[index], values[index]) && valid;
    }
    for (const char* name : kNames) unsetenv(name);
    if (!valid || values[2] <= 0.0f || values[3] <= 0.0f || values[4] < 0.0f) {
        return;
    }
    surface.hasLaunchOrigin = 1;
    surface.launchOriginX = values[0];
    surface.launchOriginY = values[1];
    surface.launchOriginWidth = values[2];
    surface.launchOriginHeight = values[3];
    surface.launchOriginCornerRadius = values[4];
}

void drawLayoutOverlay(const Widget& widget, graphics::Canvas& canvas, uint32_t depth = 0) {
    static constexpr graphics::Color kPalette[] = {
        {56, 189, 248, 220}, {74, 222, 128, 220}, {250, 204, 21, 220},
        {244, 114, 182, 220}, {167, 139, 250, 220},
    };
    const graphics::RectF bounds = widget.getAbsoluteBounds();
    if (!bounds.isEmpty()) {
        canvas.drawRoundedRect(bounds, 0.0f, {0, 0, 0, 0},
                               kPalette[depth % (sizeof(kPalette) / sizeof(kPalette[0]))],
                               1.0f, 2.0f);
    }
    for (const auto& child : widget.getChildren()) {
        drawLayoutOverlay(*child, canvas, depth + 1);
    }
}

} // namespace

struct WindowApp::PrivateRenderState {
    void reset() {
        tree = {};
        transaction = {};
        hasSnapshot = false;
        scrollTransformTransaction = false;
        retainedPresentationTransaction = false;
        externalBufferTransaction = false;
        invalidatesRetainedTemplate = false;
        retainedPresentationLayers.clear();
    }

    void synchronize(const Widget& root, bool forceReplace) {
        auto next = compiler.compile(root, tree.nodes.size());
        auto nextPresentationLayers = selectRetainedPresentationLayers(next);
        transaction = differ.diff(
            hasSnapshot ? &tree : nullptr, next, forceReplace);
        scrollTransformTransaction =
            !transaction.replacesTree && transaction.creates.empty() &&
            transaction.removals.empty() && !transaction.updates.empty() &&
            std::all_of(
                transaction.updates.begin(), transaction.updates.end(),
                [&](const auto& update) {
                    const auto* previous = findRetainedNode(tree, update.node.id);
                    return previous && !update.contentChanged &&
                        update.propertiesChanged &&
                        isScrollTranslationUpdate(update.node, *previous);
                });
        retainedPresentationTransaction =
            !transaction.replacesTree && transaction.creates.empty() &&
            transaction.removals.empty() && !transaction.updates.empty() &&
            std::all_of(
                transaction.updates.begin(), transaction.updates.end(),
                [&](const auto& update) {
                    const auto* previous = findRetainedNode(
                        tree, update.node.id);
                    if (!previous || update.contentChanged ||
                        !update.propertiesChanged) return false;
                    if (retainedPresentationLayers.contains(update.node.id) &&
                        nextPresentationLayers.contains(update.node.id)) {
                        return isRetainedPresentationUpdate(
                            update.node, *previous);
                    }
                    // Descendant screen bounds follow the cached owner's
                    // transform; their unchanged local pixels are already in
                    // that texture. Local changes still require a paint frame.
                    return isWithinRetainedPresentation(
                            tree, retainedPresentationLayers, update.node.id) &&
                        isWithinRetainedPresentation(
                            next, nextPresentationLayers, update.node.id) &&
                        isInheritedPresentationUpdate(update.node, *previous);
                });
        externalBufferTransaction =
            !transaction.replacesTree && transaction.creates.empty() &&
            transaction.removals.empty() && !transaction.updates.empty() &&
            std::all_of(
                transaction.updates.begin(), transaction.updates.end(),
                [&](const auto& update) {
                    const auto* previous = findRetainedNode(
                        tree, update.node.id);
                    return previous && !update.contentChanged &&
                        update.propertiesChanged &&
                        !isWithinScrollContent(next, update.node.id) &&
                        isExternalBufferUpdate(update.node, *previous);
                });
        invalidatesRetainedTemplate = false;
        if (!transaction.replacesTree) {
            invalidatesRetainedTemplate = std::any_of(
                transaction.creates.begin(), transaction.creates.end(),
                [&](const auto& node) {
                    if (nextPresentationLayers.contains(node.id)) return true;
                    if (isWithinRetainedPresentation(
                            next, nextPresentationLayers, node.id)) {
                        return false;
                    }
                    return isScrollBoundary(node) ||
                        !isWithinScrollContent(next, node.id);
                }) ||
                std::any_of(
                    transaction.updates.begin(), transaction.updates.end(),
                    [&](const auto& update) {
                        if (isWithinRetainedPresentation(
                                next, nextPresentationLayers,
                                update.node.id)) {
                            if (!nextPresentationLayers.contains(
                                    update.node.id)) {
                                return false;
                            }
                            if (!update.propertiesChanged) return false;
                            const auto* previous =
                                findRetainedNode(tree, update.node.id);
                            return !previous ||
                                !retainedPresentationLayers.contains(
                                    update.node.id) ||
                                !isRetainedPresentationUpdate(
                                    update.node, *previous);
                        }
                        if (isScrollViewport(update.node)) return true;
                        if (isScrollContent(update.node)) {
                            if (!update.propertiesChanged) return false;
                            const auto* previous =
                                findRetainedNode(tree, update.node.id);
                            return !previous || !isScrollTranslationUpdate(
                                update.node, *previous);
                        }
                        if (const auto* previous =
                                findRetainedNode(tree, update.node.id);
                            previous && isExternalBufferUpdate(
                                update.node, *previous)) {
                            return false;
                        }
                        return !isWithinScrollContent(next, update.node.id);
                    });
            if (!invalidatesRetainedTemplate) {
                for (uint64_t removedId : transaction.removals) {
                    const auto* removed = findRetainedNode(tree, removedId);
                    if (isWithinRetainedPresentation(
                            tree, retainedPresentationLayers, removedId)) {
                        if (retainedPresentationLayers.contains(removedId)) {
                            invalidatesRetainedTemplate = true;
                            break;
                        }
                        continue;
                    }
                    if (removed &&
                        (isScrollBoundary(*removed) ||
                         !isWithinScrollContent(tree, removedId))) {
                        invalidatesRetainedTemplate = true;
                        break;
                    }
                }
            }
        }
        tree = std::move(next);
        retainedPresentationLayers = std::move(nextPresentationLayers);
        hasSnapshot = true;
    }

    std::size_t nodeCount() const noexcept {
        return tree.nodes.size();
    }

    std::size_t retainedNodeCount() const noexcept {
        return tree.retainedNodeCount();
    }

    std::size_t createdNodeCount() const noexcept {
        return transaction.creates.size();
    }

    std::size_t contentUpdateCount() const noexcept {
        return static_cast<std::size_t>(std::count_if(
            transaction.updates.begin(), transaction.updates.end(),
            [](const auto& update) { return update.contentChanged; }));
    }

    std::size_t propertyUpdateCount() const noexcept {
        return static_cast<std::size_t>(std::count_if(
            transaction.updates.begin(), transaction.updates.end(),
            [](const auto& update) { return update.propertiesChanged; }));
    }

    std::size_t removedNodeCount() const noexcept {
        return transaction.removals.size();
    }

    const detail::RenderTreeTransaction& currentTransaction() const noexcept {
        return transaction;
    }

    bool isScrollTransformOnly() const noexcept {
        return scrollTransformTransaction;
    }

    bool isRetainedPresentationOnly() const noexcept {
        return retainedPresentationTransaction;
    }

    bool isExternalBufferOnly() const noexcept {
        return externalBufferTransaction;
    }

    bool isRetainedPresentationLayer(uint64_t id) const noexcept {
        return retainedPresentationLayers.contains(id);
    }

    bool hasRetainedPresentationLayers() const noexcept {
        return !retainedPresentationLayers.empty();
    }

    bool hasScrollContent() const noexcept {
        return std::any_of(
            tree.retainedNodes.begin(), tree.retainedNodes.end(),
            [](const auto& node) {
                return detail::hasBoundaryReason(
                    node.boundaryReasons,
                    detail::RenderBoundaryReason::ScrollContent);
            });
    }

    bool invalidatesRetainedCompositionTemplate() const noexcept {
        return invalidatesRetainedTemplate;
    }

private:
    static const detail::RetainedRenderNode* findRetainedNode(
            const detail::RenderTree& candidate, uint64_t id) noexcept {
        const auto found = std::find_if(
            candidate.retainedNodes.begin(), candidate.retainedNodes.end(),
            [id](const auto& node) { return node.id == id; });
        return found == candidate.retainedNodes.end() ? nullptr : &*found;
    }

    static bool isScrollViewport(
            const detail::RetainedRenderNode& node) noexcept {
        return detail::hasBoundaryReason(
            node.boundaryReasons,
            detail::RenderBoundaryReason::ScrollViewport);
    }

    static bool isScrollContent(
            const detail::RetainedRenderNode& node) noexcept {
        return detail::hasBoundaryReason(
            node.boundaryReasons,
            detail::RenderBoundaryReason::ScrollContent);
    }

    static bool isScrollBoundary(
            const detail::RetainedRenderNode& node) noexcept {
        return isScrollViewport(node) || isScrollContent(node);
    }

    static bool isExternalBuffer(
            const detail::RetainedRenderNode& node) noexcept {
        return detail::hasBoundaryReason(
            node.boundaryReasons,
            detail::RenderBoundaryReason::ExternalBuffer);
    }

    static bool isRetainedPresentationCandidate(
            const detail::RetainedRenderNode& node) noexcept {
        constexpr auto kRoot = detail::RenderBoundaryReason::Root;
        constexpr auto kTransform = detail::RenderBoundaryReason::Transform;
        constexpr auto kOpacity = detail::RenderBoundaryReason::Opacity;
        constexpr auto kHint =
            detail::RenderBoundaryReason::RetainedPresentation;
        const bool presentation = detail::hasBoundaryReason(
                node.boundaryReasons, kTransform) ||
            detail::hasBoundaryReason(node.boundaryReasons, kOpacity) ||
            detail::hasBoundaryReason(node.boundaryReasons, kHint);
        const float area = node.layoutBounds.width * node.layoutBounds.height;
        return presentation &&
            !detail::hasBoundaryReason(node.boundaryReasons, kRoot) &&
            !isScrollBoundary(node) &&
            !isExternalBuffer(node) &&
            node.layoutBounds.width > 0.0f &&
            node.layoutBounds.height > 0.0f &&
            node.layoutBounds.width <= 2048.0f &&
            node.layoutBounds.height <= 2048.0f && area <= 4194304.0f;
    }

    static bool isWithinScrollContent(const detail::RenderTree& candidate,
                                      uint64_t id) noexcept {
        const auto* node = findRetainedNode(candidate, id);
        std::size_t depth = 0;
        while (node && depth++ <= candidate.retainedNodes.size()) {
            if (isScrollContent(*node)) return true;
            if (node->parentId == 0) return false;
            node = findRetainedNode(candidate, node->parentId);
        }
        return false;
    }

    static bool descendsFrom(
            const detail::RenderTree& candidate, uint64_t id,
            uint64_t ancestorId) noexcept {
        const auto* node = findRetainedNode(candidate, id);
        std::size_t depth = 0;
        while (node && depth++ <= candidate.retainedNodes.size()) {
            if (node->parentId == ancestorId) return true;
            if (node->parentId == 0) return false;
            node = findRetainedNode(candidate, node->parentId);
        }
        return false;
    }

    static std::unordered_set<uint64_t> selectRetainedPresentationLayers(
            const detail::RenderTree& candidate) {
        if constexpr (!kRetainedPresentationCacheEnabled) {
            return {};
        }
        std::unordered_set<uint64_t> result;
        for (const auto& node : candidate.retainedNodes) {
            if (!isRetainedPresentationCandidate(node) ||
                isWithinScrollContent(candidate, node.id)) {
                continue;
            }
            bool hasCandidateAncestor = false;
            for (const auto& ancestor : candidate.retainedNodes) {
                if (ancestor.id != node.id &&
                    isRetainedPresentationCandidate(ancestor) &&
                    descendsFrom(candidate, node.id, ancestor.id)) {
                    hasCandidateAncestor = true;
                    break;
                }
            }
            if (hasCandidateAncestor) continue;

            const bool containsScroll = std::any_of(
                candidate.retainedNodes.begin(),
                candidate.retainedNodes.end(), [&](const auto& descendant) {
                    return isScrollBoundary(descendant) &&
                        descendsFrom(candidate, descendant.id, node.id);
                });
            const bool containsExternal = std::any_of(
                candidate.retainedNodes.begin(),
                candidate.retainedNodes.end(), [&](const auto& descendant) {
                    return isExternalBuffer(descendant) &&
                        (descendant.id == node.id || descendsFrom(
                            candidate, descendant.id, node.id));
                });
            if (!containsScroll && !containsExternal) result.insert(node.id);
        }
        return result;
    }

    static bool isWithinRetainedPresentation(
            const detail::RenderTree& candidate,
            const std::unordered_set<uint64_t>& layers,
            uint64_t id) noexcept {
        return std::any_of(
            layers.begin(), layers.end(), [&](uint64_t layer) {
                return layer == id || descendsFrom(candidate, id, layer);
            });
    }

    static bool sameRect(const graphics::RectF& lhs,
                         const graphics::RectF& rhs) noexcept {
        return lhs.x == rhs.x && lhs.y == rhs.y &&
            lhs.width == rhs.width && lhs.height == rhs.height;
    }

    static bool sameOptionalRect(
            const std::optional<graphics::RectF>& lhs,
            const std::optional<graphics::RectF>& rhs) noexcept {
        return lhs.has_value() == rhs.has_value() &&
            (!lhs || sameRect(*lhs, *rhs));
    }

    static bool isScrollTranslationUpdate(
            const detail::RetainedRenderNode& node,
            const detail::RetainedRenderNode& previous) noexcept {
        constexpr auto kTransform =
            detail::RenderBoundaryReason::Transform;
        constexpr float kEpsilon = 0.0001f;
        const auto same = [=](float lhs, float rhs) {
            return std::fabs(lhs - rhs) <= kEpsilon;
        };
        const auto reasonsChanged = static_cast<uint32_t>(
            node.boundaryReasons) ^ static_cast<uint32_t>(
            previous.boundaryReasons);
        const float translationDeltaX = node.presentation.translationX -
            previous.presentation.translationX;
        const float translationDeltaY = node.presentation.translationY -
            previous.presentation.translationY;
        return isScrollContent(node) && isScrollContent(previous) &&
            (reasonsChanged & ~static_cast<uint32_t>(kTransform)) == 0 &&
            node.parentId == previous.parentId &&
            node.siblingIndex == previous.siblingIndex &&
            sameRect(node.layoutBounds, previous.layoutBounds) &&
            same(node.presentationBounds.x - previous.presentationBounds.x,
                 translationDeltaX) &&
            same(node.presentationBounds.y - previous.presentationBounds.y,
                 translationDeltaY) &&
            same(node.presentationBounds.width,
                 previous.presentationBounds.width) &&
            same(node.presentationBounds.height,
                 previous.presentationBounds.height) &&
            sameOptionalRect(node.clipBounds, previous.clipBounds) &&
            same(node.presentation.opacity, 1.0f) &&
            same(previous.presentation.opacity, 1.0f) &&
            same(node.presentation.scaleX, 1.0f) &&
            same(node.presentation.scaleY, 1.0f) &&
            same(previous.presentation.scaleX, 1.0f) &&
            same(previous.presentation.scaleY, 1.0f) &&
            same(node.presentation.rotationRadians, 0.0f) &&
            same(previous.presentation.rotationRadians, 0.0f) &&
            same(node.presentation.originX, previous.presentation.originX) &&
            same(node.presentation.originY, previous.presentation.originY) &&
            node.externalBufferId == previous.externalBufferId &&
            node.externalBufferRevision ==
                previous.externalBufferRevision &&
            node.children == previous.children;
    }

    static bool isRetainedPresentationUpdate(
            const detail::RetainedRenderNode& node,
            const detail::RetainedRenderNode& previous) noexcept {
        constexpr auto kTransform = detail::RenderBoundaryReason::Transform;
        constexpr auto kOpacity = detail::RenderBoundaryReason::Opacity;
        constexpr auto kHint =
            detail::RenderBoundaryReason::RetainedPresentation;
        const auto allowedReasons = static_cast<uint32_t>(kTransform) |
            static_cast<uint32_t>(kOpacity) |
            static_cast<uint32_t>(kHint);
        const auto reasonsChanged = static_cast<uint32_t>(
            node.boundaryReasons) ^ static_cast<uint32_t>(
            previous.boundaryReasons);
        return !isScrollBoundary(node) && !isScrollBoundary(previous) &&
            (reasonsChanged & ~allowedReasons) == 0 &&
            node.parentId == previous.parentId &&
            node.siblingIndex == previous.siblingIndex &&
            sameRect(node.layoutBounds, previous.layoutBounds) &&
            sameRetainedClip(node, previous) &&
            node.children == previous.children;
    }

    static bool sameRetainedClip(
            const detail::RetainedRenderNode& node,
            const detail::RetainedRenderNode& previous) noexcept {
        if (sameOptionalRect(node.clipBounds, previous.clipBounds)) return true;
        // clipsToBounds is recorded inside the identity-space cached layer.
        // Its screen-space envelope moving with the owner is not a new clip.
        return node.clipBounds && previous.clipBounds &&
            sameRect(*node.clipBounds, node.presentationBounds) &&
            sameRect(*previous.clipBounds, previous.presentationBounds);
    }

    static bool isInheritedPresentationUpdate(
            const detail::RetainedRenderNode& node,
            const detail::RetainedRenderNode& previous) noexcept {
        const auto& p = node.presentation;
        const auto& old = previous.presentation;
        return node.propertyRevision == previous.propertyRevision &&
            node.boundaryReasons == previous.boundaryReasons &&
            node.parentId == previous.parentId &&
            node.siblingIndex == previous.siblingIndex &&
            sameRect(node.layoutBounds, previous.layoutBounds) &&
            sameRetainedClip(node, previous) &&
            node.children == previous.children &&
            node.externalBufferId == previous.externalBufferId &&
            node.externalBufferRevision == previous.externalBufferRevision &&
            p.opacity == old.opacity && p.translationX == old.translationX &&
            p.translationY == old.translationY && p.scaleX == old.scaleX &&
            p.scaleY == old.scaleY && p.rotationRadians == old.rotationRadians &&
            p.originX == old.originX && p.originY == old.originY;
    }

    static bool isExternalBufferUpdate(
            const detail::RetainedRenderNode& node,
            const detail::RetainedRenderNode& previous) noexcept {
        return isExternalBuffer(node) && isExternalBuffer(previous) &&
            node.parentId == previous.parentId &&
            node.siblingIndex == previous.siblingIndex &&
            node.boundaryReasons == previous.boundaryReasons &&
            sameRect(node.layoutBounds, previous.layoutBounds) &&
            sameRect(node.presentationBounds, previous.presentationBounds) &&
            sameOptionalRect(node.clipBounds, previous.clipBounds) &&
            node.presentation.opacity == previous.presentation.opacity &&
            node.presentation.translationX == previous.presentation.translationX &&
            node.presentation.translationY == previous.presentation.translationY &&
            node.presentation.scaleX == previous.presentation.scaleX &&
            node.presentation.scaleY == previous.presentation.scaleY &&
            node.presentation.rotationRadians ==
                previous.presentation.rotationRadians &&
            node.presentation.originX == previous.presentation.originX &&
            node.presentation.originY == previous.presentation.originY &&
            node.children == previous.children &&
            (node.externalBufferId != previous.externalBufferId ||
             node.externalBufferRevision !=
                 previous.externalBufferRevision);
    }

    detail::RenderNodeCompiler compiler;
    detail::RenderTreeDiffer differ;
    detail::RenderTree tree;
    detail::RenderTreeTransaction transaction;
    bool hasSnapshot{false};
    bool scrollTransformTransaction{false};
    bool retainedPresentationTransaction{false};
    bool externalBufferTransaction{false};
    bool invalidatesRetainedTemplate{false};
    std::unordered_set<uint64_t> retainedPresentationLayers;
};

WindowApp::WindowApp(std::unique_ptr<graphics::Canvas> canvas, float width, float height,
                     const std::string& title)
    : m_width(width), m_height(height), m_title(title),
      m_privateRenderState(std::make_unique<PrivateRenderState>()),
      m_canvas(std::move(canvas)),
      m_rasterClient(std::make_unique<RasterServiceClient>()) {
    setupAppSignalHandlers();
    updateLayoutEnvironment();
    if (!m_canvas) return;
    m_frameTraceEnabled = frameTraceEnabled();
    m_layoutOverlayEnabled = layoutOverlayEnabled();
    m_traceLastLog = std::chrono::steady_clock::now();
    const uint32_t pixelWidth = toBufferPixels(width, m_bufferScale);
    const uint32_t pixelHeight = toBufferPixels(height, m_bufferScale);
    if (!m_canvas->usesDisplayListTransport()) {
        m_pixelBuffer.resize(
            static_cast<size_t>(pixelWidth) * pixelHeight, 0xFF000000);
    }
    m_initialized = m_canvas->initialize(
        pixelWidth, pixelHeight,
        m_pixelBuffer.empty() ? nullptr : m_pixelBuffer.data());
    updateCanvasRenderTarget();
    m_motionCoordinator.setLayoutCallback([this] { updateLayout(); });

    auto defaultRoot = std::make_unique<Container>();
    defaultRoot->setWidth(static_cast<float>(width));
    defaultRoot->setHeight(static_cast<float>(height));
    setRootWidget(std::move(defaultRoot));
    m_lastResizeApply = std::chrono::steady_clock::now();
    m_lastAnimationTick = std::chrono::steady_clock::now();
}

WindowApp::~WindowApp() {
    m_running = false;
    m_lifetimeToken.reset();
    // Widgets unregister their channels on destruction. Detach while the
    // coordinator member is still alive (member teardown runs in reverse).
    m_transients.clear();
    m_transients.setWindowRoot(nullptr);
    m_hostedSurfaces.clear();
    if (m_windowRoot) m_windowRoot->setMotionCoordinator(nullptr);
    if (m_socketFd >= 0 && m_ownsSocketFd) {
        lcl::protocol::discardPendingWrites(m_socketFd);
        close(m_socketFd);
        m_socketFd = -1;
    }
}

uint32_t WindowApp::getPixelWidth() const {
    return toBufferPixels(m_width, m_bufferScale);
}

uint32_t WindowApp::getPixelHeight() const {
    return toBufferPixels(m_height, m_bufferScale);
}

void WindowApp::updateCanvasRenderTarget() {
    if (!m_canvas) return;
    m_canvas->setRenderTarget({
        {static_cast<float>(m_width), static_cast<float>(m_height)},
        {getPixelWidth(), getPixelHeight()},
        m_bufferScale,
    });
}

bool WindowApp::setResizeConstraints(WindowResizeConstraints constraints) {
    constexpr float kMaxLogicalExtent = 16384.0f;
    const auto valid = [kMaxLogicalExtent](float value) {
        return std::isfinite(value) && value >= 0.0f &&
            value <= kMaxLogicalExtent;
    };
    if (m_ipcConnected || isPopupSurface() || isAttachedSurface() ||
        !valid(constraints.baseWidth) ||
        !valid(constraints.baseHeight) ||
        !valid(constraints.widthIncrement) ||
        !valid(constraints.heightIncrement)) {
        return false;
    }
    m_resizeConstraints = constraints;
    return true;
}

void WindowApp::setRootWidget(std::unique_ptr<Widget> root) {
    if (!root) return;
    m_dispatcher.cancelPointerCaptures();
    m_transients.setWindowRoot(nullptr);

    auto windowRoot = std::make_unique<Container>();
    windowRoot->setThemeContext(&m_themeContext);
    windowRoot->setWidth(static_cast<float>(m_width));
    windowRoot->setHeight(static_cast<float>(m_height));
    // A normal application root represents the complete client surface.
    // App-provided startup dimensions must not remain as fixed cross-axis
    // constraints after the compositor configures a new window size. Absolute
    // roots are intentional surface fragments (for example a titlebar-only
    // test host) and keep their own geometry.
    if (root->positionType() != layout::PositionType::Absolute) {
        root->setWidthAuto();
        root->setHeightAuto();
        root->setAlignSelf(layout::Align::Stretch);
    }
    root->setFlexGrow(1.0f);
    root->setFlexShrink(1.0f);
    Widget* contentRoot = root.get();
    windowRoot->addChild(std::move(root));
    windowRoot->setRenderPass(&m_renderPass);
    windowRoot->setMotionCoordinator(&m_motionCoordinator);

    m_windowRoot = std::move(windowRoot);
    m_rootWidget = contentRoot;
    m_privateRenderState->reset();
    m_scrollTransformFastPathReady = false;
    m_retainedPresentationFastPathReady = false;
    m_externalBufferFastPathReady = false;
    m_transients.setWindowRoot(m_windowRoot.get());
    m_windowRoot->markLayoutDirty();
    m_windowRoot->invalidatePaint();
    // A newly mounted tree has not been through layout/sync yet, so its
    // absolute bounds are still empty and invalidatePaint() cannot produce damage.
    // Force one full frame after layout; runtime root replacement must not
    // leave old pixels in the client buffer.
    m_firstFrame = true;
}

void WindowApp::setTheme(lcl::theme::Theme theme) {
    m_themeContext.setTheme(std::move(theme));
    if (m_windowRoot) {
        m_windowRoot->setThemeContext(&m_themeContext);
        m_windowRoot->markLayoutDirty();
        m_windowRoot->invalidatePaint();
    }
    m_firstFrame = true;
}

TransientHandle WindowApp::registerLocalTransient(std::unique_ptr<Widget> widget,
                                                  TransientOptions options) {
    if (!widget) return 0;
    widget->setPositionType(layout::PositionType::Absolute);
    return m_transients.registerLocal(std::move(widget), std::move(options));
}

TransientHandle WindowApp::registerSurfaceTransient(
        uint32_t surfaceId, std::function<void()> destroySurface,
        TransientOptions options) {
    return m_transients.registerSurface(surfaceId, std::move(destroySurface),
                                        std::move(options));
}

TransientHandle WindowApp::registerSurfaceTransient(
        uint32_t surfaceId, TransientOptions options) {
    return m_transients.registerSurface(
        surfaceId,
        [this, surfaceId] { requestSurfaceDestroy(surfaceId); },
        std::move(options));
}

bool WindowApp::removeTransient(TransientHandle handle) {
    return m_transients.remove(handle);
}

void WindowApp::clearTransients() {
    m_transients.clear();
}

HostedSurfaceHandle WindowApp::hostSurface(
        std::unique_ptr<WindowApp> surface, std::function<void()> onClosed) {
    if (!surface || surface.get() == this) return 0;
    const HostedSurfaceHandle handle = m_nextHostedSurfaceHandle++;
    m_hostedSurfaces.push_back(HostedSurfaceEntry{
        handle, std::move(surface), std::move(onClosed), false});
    return handle;
}

bool WindowApp::removeHostedSurface(HostedSurfaceHandle handle) {
    const auto found = std::find_if(
        m_hostedSurfaces.begin(), m_hostedSurfaces.end(),
        [handle](const HostedSurfaceEntry& entry) { return entry.handle == handle; });
    if (found == m_hostedSurfaces.end()) return false;
    if (found->pendingRemoval) return true;
    found->pendingRemoval = true;
    if (found->surface) found->surface->requestWindowClose();
    if (!m_tickingHostedSurfaces) collectClosedHostedSurfaces();
    return true;
}

WindowApp* WindowApp::getHostedSurface(HostedSurfaceHandle handle) const noexcept {
    const auto found = std::find_if(
        m_hostedSurfaces.begin(), m_hostedSurfaces.end(),
        [handle](const HostedSurfaceEntry& entry) { return entry.handle == handle; });
    return found == m_hostedSurfaces.end() ? nullptr : found->surface.get();
}

void WindowApp::setInputEnabled(bool enabled) {
    if (m_inputEnabled == enabled) return;
    if (!enabled) m_dispatcher.cancelPointerCaptures();
    m_inputEnabled = enabled;
}

bool WindowApp::connectCompositor(const std::string& socketPath) {
    if (m_appId.empty()) {
        std::cerr << "[lcl-ui ERROR] WindowApp requires a canonical app ID before connection\n";
        return false;
    }
    if (!m_canvas || !m_canvas->usesDisplayListTransport()) {
        std::cerr << "[lcl-ui ERROR] Protocol v27 requires the retained DisplayList canvas\n";
        return false;
    }
    std::string effectiveSocketPath = socketPath;
    if (const char* envSocket = std::getenv("LCL_COMPOSITOR_SOCKET")) {
        if (envSocket[0] != '\0') {
            effectiveSocketPath = envSocket;
        }
    }
    for (int i = 0; i < 50; ++i) {
        m_socketFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (m_socketFd >= 0) {
            struct sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, effectiveSocketPath.c_str(), sizeof(addr.sun_path) - 1);
            if (connect(m_socketFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
                break;
            }
            close(m_socketFd);
            m_socketFd = -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (m_socketFd < 0) {
        std::cerr << "[lcl-ui ERROR] Could not connect to compositor IPC socket: " << effectiveSocketPath << "\n";
        return false;
    }
    m_compositorSocketPath = effectiveSocketPath;
    m_surfaceEnded = false;

    // The compositor is the output-scale authority. Initial creation is purely
    // logical; ConfigureBounds supplies the buffer mapping scale.
    m_bufferScale = 1.0f;
    updateCanvasRenderTarget();

    // Set non-blocking socket reads
    int flags = fcntl(m_socketFd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(m_socketFd, F_SETFL, flags | O_NONBLOCK);
    }

    if (!isPopupSurface() && !isAttachedSurface() &&
        m_systemSurfaceKind != lcl::protocol::LCLSystemSurfaceKind::None) {
        lcl::protocol::LCLMsgSetSystemSurfaceKind systemSurface{};
        systemSurface.kind = m_systemSurfaceKind;
        if (!sendProtocolMessage(lcl::protocol::LCLOpcode::SetSystemSurfaceKind,
                                 &systemSurface, sizeof(systemSurface))) {
            std::cerr << "[lcl-ui ERROR] Failed to declare system-surface policy\n";
            return false;
        }
    }

    // 2. Request a normal, popup, or WM-owned attached surface. All three
    // continue through the same configure, raster grant, render, and input
    // paths; the compositor only receives generic scene relationships here.
    m_waitingForInitialConfigure = true;
    bool createSent = false;
    if (isAttachedSurface()) {
        lcl::protocol::LCLMsgAttachedSurfaceCreate attached{};
        attached.surfaceId = m_surfaceId;
        attached.targetWindowId = m_attachedWindowId;
        attached.role = m_attachedRole;
        attached.x = m_attachedX;
        attached.y = m_attachedY;
        attached.width = m_attachedWidth;
        attached.height = m_attachedHeight;
        attached.followParentWidth = m_attachedFollowParentWidth ? 1 : 0;
        attached.followParentHeight = m_attachedFollowParentHeight ? 1 : 0;
        attached.acceptsInput = m_attachedAcceptsInput ? 1 : 0;
        createSent = sendProtocolMessage(
            lcl::protocol::LCLOpcode::AttachedSurfaceCreate,
            &attached, sizeof(attached));
    } else if (isPopupSurface()) {
        lcl::protocol::LCLMsgPopupSurfaceCreate popup{};
        popup.surfaceId = m_surfaceId;
        popup.parentSurfaceId = m_popupParentSurfaceId;
        popup.role = m_popupRole;
        popup.x = m_popupX;
        popup.y = m_popupY;
        popup.width = m_width;
        popup.height = m_height;
        createSent = sendProtocolMessage(lcl::protocol::LCLOpcode::PopupSurfaceCreate,
                                         &popup, sizeof(popup));
    } else {
        lcl::protocol::LCLMsgSurfaceCreate surface{};
        surface.surfaceId = m_surfaceId;
        surface.x = m_initialX;
        surface.y = m_initialY;
        surface.width = m_width;
        surface.height = m_height;
        surface.resizeBaseWidth = m_resizeConstraints.baseWidth;
        surface.resizeBaseHeight = m_resizeConstraints.baseHeight;
        surface.resizeWidthIncrement = m_resizeConstraints.widthIncrement;
        surface.resizeHeightIncrement = m_resizeConstraints.heightIncrement;
        std::strncpy(surface.title, m_title.c_str(), sizeof(surface.title) - 1);
        std::strncpy(surface.appId, m_appId.c_str(), sizeof(surface.appId) - 1);
        takeLaunchOrigin(surface);
        createSent = sendProtocolMessage(lcl::protocol::LCLOpcode::SurfaceCreate,
                                         &surface, sizeof(surface));
    }
    if (!createSent) {
        m_waitingForInitialConfigure = false;
        std::cerr << "[lcl-ui ERROR] Failed to create surface\n";
        return false;
    }

    m_ipcConnected = true;
    m_ownsSocketFd = true;
    m_uploadedImageRevisions.clear();
    m_uploadedExternalBuffers.clear();
    m_backingWidth = m_width;
    m_backingHeight = m_height;
    // Decoration state belongs to the surface contract, not to a later frame.
    // Send preconfigured values before the first effect graph/buffer attach so a
    // CSD client never flashes the compositor's default title chrome.
    if (!isPopupSurface() && !isAttachedSurface() && m_hasRequestedDecorationMode) {
        setDecorationMode(m_requestedDecorationMode);
    }
    if (!isPopupSurface() && !isAttachedSurface() && m_hasRequestedEdgeToEdge) {
        setEdgeToEdge(m_requestedEdgeToEdge);
    }
    if (!isAttachedSurface() && m_hasRequestedCornerRadius) {
        setWindowCornerStyle(m_requestedCornerRadius, m_requestedCornerRoundness);
    }
    if (m_windowRoot) m_windowRoot->invalidatePaint();
    m_firstFrame = true;

    std::cout << "[lcl-ui] Connected to Compositor IPC socket successfully (" << m_title << ").\n";
    return true;
}

void WindowApp::resize(float width, float height) {
    if (width == 0 || height == 0) return;
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;
    updateLayoutEnvironment();

    if (m_windowRoot) {
        m_windowRoot->setWidth(static_cast<float>(width));
        m_windowRoot->setHeight(static_cast<float>(height));
    }

    if (m_ipcConnected) {
        updateCanvasRenderTarget();
        // Defer render+attach to the main loop's renderFrame() so each resize tick
        // produces at most one frame and one attach commit.
        m_firstFrame = true;
    }

    if (m_onResize) {
        m_onResize(width, height);
    }
}

void WindowApp::setSafeAreaInsets(LayoutInsets insets) {
    insets = sanitizeLayoutInsets(insets);
    if (m_safeAreaInsets == insets) return;
    m_safeAreaInsets = insets;
    updateLayoutEnvironment();
}

void WindowApp::setLayoutSizeClassPolicy(LayoutSizeClassPolicy policy) {
    policy = sanitizeLayoutSizeClassPolicy(policy);
    if (m_layoutSizeClassPolicy == policy) return;
    m_layoutSizeClassPolicy = policy;
    updateLayoutEnvironment();
}

void WindowApp::updateLayoutEnvironment() {
    const LayoutEnvironment next = makeLayoutEnvironment(
        m_width, m_height, m_safeAreaInsets, m_layoutSizeClassPolicy);
    if (next == m_layoutEnvironment) return;
    m_layoutEnvironment = next;
    if (m_onLayoutEnvironmentChanged) {
        m_onLayoutEnvironmentChanged(m_layoutEnvironment);
    }
}

void WindowApp::setInitialBounds(float x, float y, float width, float height) {
    if (m_ipcConnected || width == 0 || height == 0) return;

    m_initialX = x;
    m_initialY = y;
    m_width = width;
    m_height = height;
    updateLayoutEnvironment();
    const uint32_t pixelWidth = toBufferPixels(width, m_bufferScale);
    const uint32_t pixelHeight = toBufferPixels(height, m_bufferScale);
    if (!m_canvas->usesDisplayListTransport()) {
        m_pixelBuffer.resize(
            static_cast<size_t>(pixelWidth) * pixelHeight, 0xFF000000);
        m_canvas->setTargetPixels(m_pixelBuffer.data(), pixelWidth, pixelHeight);
    }
    updateCanvasRenderTarget();
    if (m_windowRoot) {
        m_windowRoot->setWidth(static_cast<float>(width));
        m_windowRoot->setHeight(static_cast<float>(height));
    }
}

void WindowApp::configurePopupSurface(uint32_t parentSurfaceId,
                                      lcl::protocol::LCLPopupRole role,
                                      float x, float y) {
    if (m_ipcConnected || parentSurfaceId == 0 || isAttachedSurface()) return;
    m_popupParentSurfaceId = parentSurfaceId;
    m_popupRole = role;
    m_popupX = x;
    m_popupY = y;
}

void WindowApp::configureAttachedSurface(
        uint32_t targetWindowId,
        lcl::protocol::LCLAttachedSurfaceRole role,
        float x, float y, float width, float height,
        bool followParentWidth, bool followParentHeight,
        bool acceptsInput) {
    if (m_ipcConnected || targetWindowId == 0 || isPopupSurface() ||
        width <= 0.0f || height <= 0.0f) return;

    m_attachedWindowId = targetWindowId;
    m_attachedRole = role;
    m_attachedX = x;
    m_attachedY = y;
    m_attachedWidth = width;
    m_attachedHeight = height;
    m_attachedFollowParentWidth = followParentWidth;
    m_attachedFollowParentHeight = followParentHeight;
    m_attachedAcceptsInput = acceptsInput;
    setInitialBounds(x, y, width, height);
}

void WindowApp::pollIPC() {
    if (!m_ipcConnected || m_socketFd < 0) return;

    const bool rasterWasConnected = m_rasterClient->isConnected();
    for (const auto& discarded : m_rasterClient->pollDiscards()) {
        if (discarded.surfaceId != m_surfaceId ||
            discarded.frameSerial != m_submittedFrameSerial) continue;
        m_submittedConfigureSerial = 0;
        m_submittedFrameSerial = 0;
        m_submittedGeometryGeneration = 0;
        m_retainedRasterFrameSerial = 0;
        m_scrollTransformFastPathReady = false;
        m_retainedPresentationFastPathReady = false;
        m_externalBufferFastPathReady = false;
        m_frameGateOpen = true;
        m_firstFrame = true;
    }
    for (const auto& released :
         m_rasterClient->takeExternalBufferReleases()) {
        const auto found = m_uploadedExternalBuffers.find(
            released.message.bufferId);
        if (found != m_uploadedExternalBuffers.end() &&
            found->second.contentRevision ==
                released.message.contentRevision) {
            if (found->second.completeRelease) {
                found->second.completeRelease(released.releaseFenceFd);
            } else if (released.releaseFenceFd >= 0) {
                close(released.releaseFenceFd);
            }
            m_uploadedExternalBuffers.erase(found);
        } else if (released.releaseFenceFd >= 0) {
            close(released.releaseFenceFd);
        }
    }
    if (rasterWasConnected && !m_rasterClient->isConnected()) {
        // rasterd is supervised independently. Keep compositor IPC and input
        // alive, restore frame credit, then resend the complete retained app
        // frame/resource state when the daemon's socket returns.
        m_uploadedImageRevisions.clear();
        m_uploadedExternalBuffers.clear();
        m_submittedConfigureSerial = 0;
        m_submittedFrameSerial = 0;
        m_submittedGeometryGeneration = 0;
        m_retainedRasterFrameSerial = 0;
        m_scrollTransformFastPathReady = false;
        m_retainedPresentationFastPathReady = false;
        m_externalBufferFastPathReady = false;
        m_frameGateOpen = true;
        m_firstFrame = true;
    }

    float latestWidth = 0.0f;
    float latestHeight = 0.0f;
    float latestBackingWidth = 0.0f;
    float latestBackingHeight = 0.0f;
    float latestScale = m_bufferScale;
    uint64_t latestGeometryGeneration = m_geometryGeneration;
    lcl::protocol::LCLConfigureResizeReason latestResizeReason =
        lcl::protocol::LCLConfigureResizeReason::Initial;
    bool pendingResize = false;
    bool receivedInitialConfigure = false;

    while (true) {
        lcl::protocol::LCLHeader header{};
        std::vector<uint8_t> payload;
        int receivedFd = -1;
        const auto receiveStatus =
            lcl::protocol::recvPacketWithFd(m_socketFd, header, payload, receivedFd);
        if (receiveStatus == lcl::protocol::ReceiveStatus::Received) {
            bool receivedFdConsumed = false;
            if (header.opcode == lcl::protocol::LCLOpcode::InputEvent &&
                payload.size() >= sizeof(lcl::protocol::LCLMsgInputEvent)) {
                auto* inputMsg = reinterpret_cast<const lcl::protocol::LCLMsgInputEvent*>(payload.data());
                if (inputMsg->surfaceId != m_surfaceId) continue;
                // A shell panel can be visually topmost without being interactive.
                // Do not let hover handling mutate its widget tree until it explicitly
                // opts into input (dock activation is intentionally future work).
                if (!m_inputEnabled) continue;
                if (inputMsg->type == static_cast<uint32_t>(lcl::protocol::LCLInputEventType::KeyDown)) {
                    sendKeyDown(inputMsg->key, static_cast<char32_t>(inputMsg->codepoint), inputMsg->modifiers);
                } else if (inputMsg->type == static_cast<uint32_t>(lcl::protocol::LCLInputEventType::KeyUp)) {
                    sendKeyUp(inputMsg->key, inputMsg->modifiers);
                } else if (inputMsg->type == static_cast<uint32_t>(lcl::protocol::LCLInputEventType::PointerMotion)) {
                    const auto source = (inputMsg->source == static_cast<uint8_t>(lcl::protocol::LCLPointerSource::Touch))
                        ? PointerSource::Touch
                        : PointerSource::Mouse;
                    sendPointerMove(inputMsg->x, inputMsg->y, source, inputMsg->pointerId);
                } else if (inputMsg->type == static_cast<uint32_t>(lcl::protocol::LCLInputEventType::PointerButton)) {
                    const auto source = (inputMsg->source == static_cast<uint8_t>(lcl::protocol::LCLPointerSource::Touch))
                        ? PointerSource::Touch
                        : PointerSource::Mouse;
                    if (inputMsg->pressed) {
                        sendPointerDown(inputMsg->x, inputMsg->y, inputMsg->key, source,
                                        inputMsg->pointerId);
                    } else {
                        sendPointerUp(inputMsg->x, inputMsg->y, inputMsg->key, source,
                                      inputMsg->pointerId);
                    }
                } else if (inputMsg->type == static_cast<uint32_t>(lcl::protocol::LCLInputEventType::PointerCancel)) {
                    const auto source = (inputMsg->source == static_cast<uint8_t>(lcl::protocol::LCLPointerSource::Touch))
                        ? PointerSource::Touch
                        : PointerSource::Mouse;
                    sendPointerCancel(inputMsg->x, inputMsg->y, source,
                                      inputMsg->pointerId);
                } else if (inputMsg->type == static_cast<uint32_t>(lcl::protocol::LCLInputEventType::PointerScroll)) {
                    const auto source = (inputMsg->source == static_cast<uint8_t>(lcl::protocol::LCLPointerSource::Touch))
                        ? PointerSource::Touch
                        : PointerSource::Mouse;
                    sendPointerScroll(inputMsg->x, inputMsg->y, inputMsg->deltaX,
                                      inputMsg->deltaY, source, inputMsg->pointerId);
                } else if (inputMsg->type == static_cast<uint32_t>(lcl::protocol::LCLInputEventType::TextInput)) {
                    if (inputMsg->codepoint > 0) {
                        std::string utf8;
                        char32_t cp = inputMsg->codepoint;
                        if (cp <= 0x7F) {
                            utf8.push_back(static_cast<char>(cp));
                        } else if (cp <= 0x7FF) {
                            utf8.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
                            utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else if (cp <= 0xFFFF) {
                            utf8.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
                            utf8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                        sendTextInput(utf8);
                    }
                }
            } else if (header.opcode == lcl::protocol::LCLOpcode::ConfigureBounds &&
                       payload.size() == sizeof(lcl::protocol::LCLMsgConfigureBounds)) {
                auto* cfg = reinterpret_cast<const lcl::protocol::LCLMsgConfigureBounds*>(payload.data());
                if (cfg->surfaceId != m_surfaceId) continue;
                if (cfg->width > 0 && cfg->height > 0) {
                    if (m_submittedFrameSerial != 0 &&
                        cfg->geometryGeneration >
                            m_submittedGeometryGeneration) {
                        // A newer geometry target invalidates the older raster
                        // credit locally. rasterd/compositor discard that late
                        // layer by serial; the client may immediately produce
                        // the coalesced latest generation.
                        m_submittedConfigureSerial = 0;
                        m_submittedFrameSerial = 0;
                        m_submittedGeometryGeneration = 0;
                        m_retainedRasterFrameSerial = 0;
                        m_scrollTransformFastPathReady = false;
                        m_retainedPresentationFastPathReady = false;
                        m_externalBufferFastPathReady = false;
                        m_frameGateOpen = true;
                        m_firstFrame = true;
                    }
                    if (m_frameTraceEnabled) {
                        ++m_traceConfigureCount;
                        if (cfg->resizeReason == lcl::protocol::LCLConfigureResizeReason::Interactive) {
                            ++m_traceInteractiveConfigureCount;
                        } else if (cfg->resizeReason == lcl::protocol::LCLConfigureResizeReason::WindowStateTransition) {
                            ++m_traceTransitionConfigureCount;
                        }
                    }
                    latestWidth = cfg->width;
                    latestHeight = cfg->height;
                    latestBackingWidth = cfg->backingWidth;
                    latestBackingHeight = cfg->backingHeight;
                    latestScale = cfg->bufferScale;
                    latestResizeReason = cfg->resizeReason;
                    m_pendingConfigureSerial = cfg->configureSerial;
                    latestGeometryGeneration = cfg->geometryGeneration;
                    receivedInitialConfigure = receivedInitialConfigure || m_waitingForInitialConfigure;
                    pendingResize = true;
                }
            } else if (header.opcode ==
                           lcl::protocol::LCLOpcode::SurfaceProducerGrant &&
                       payload.size() == sizeof(
                           lcl::protocol::LCLMsgSurfaceProducerGrant)) {
                const auto* grant = reinterpret_cast<const
                    lcl::protocol::LCLMsgSurfaceProducerGrant*>(payload.data());
                if (grant->surfaceId == m_surfaceId) {
                    raster_protocol::SurfaceGrant rasterGrant{};
                    rasterGrant.surfaceId = grant->surfaceId;
                    rasterGrant.ownerPid = grant->ownerPid;
                    rasterGrant.flags = grant->flags;
                    rasterGrant.tokenHigh = grant->tokenHigh;
                    rasterGrant.tokenLow = grant->tokenLow;
                    const auto runtimeDirectory = std::filesystem::path(
                        m_compositorSocketPath).parent_path();
                    m_rasterClient->configure(
                        (runtimeDirectory / "lcl-raster.sock").string(),
                        rasterGrant);
                    m_uploadedImageRevisions.clear();
                    m_uploadedExternalBuffers.clear();
                    m_rasterConnectionGeneration = 0;
                    m_retainedRasterFrameSerial = 0;
                    m_scrollTransformFastPathReady = false;
                    m_retainedPresentationFastPathReady = false;
                    m_externalBufferFastPathReady = false;
                    m_firstFrame = true;
                }
            } else if (header.opcode == lcl::protocol::LCLOpcode::FramePresented &&
                       payload.size() == sizeof(lcl::protocol::LCLMsgFramePresented)) {
                const auto* presented = reinterpret_cast<const lcl::protocol::LCLMsgFramePresented*>(payload.data());
                if (presented->surfaceId == m_surfaceId) {
                    if (m_submittedFrameSerial != 0 &&
                        presented->frameSerial != m_submittedFrameSerial) {
                        continue;
                    }
                    m_lastPresentedTimestampNs = presented->timestampNs;
                    m_refreshIntervalNs = presented->refreshIntervalNs;
                    m_retainedRasterFrameSerial = presented->frameSerial;
                    m_submittedConfigureSerial = 0;
                    m_submittedFrameSerial = 0;
                    m_submittedGeometryGeneration = 0;
                    m_frameGateOpen = true;
                    if (m_frameTraceEnabled) {
                        ++m_tracePresentedFrames;
                        const auto appendDuration = [](uint64_t startNs,
                                                       uint64_t endNs,
                                                       std::vector<double>& samples) {
                            if (startNs == 0 || endNs < startNs) return;
                            samples.push_back(static_cast<double>(endNs - startNs) /
                                              1'000'000.0);
                        };
                        appendDuration(presented->clientFrameStartNs,
                                       presented->clientSubmitNs,
                                       m_traceClientBuildSamplesMs);
                        appendDuration(presented->clientSubmitNs,
                                       presented->rasterStartNs,
                                       m_traceRasterQueueSamplesMs);
                        appendDuration(presented->rasterStartNs,
                                       presented->rasterReadyNs,
                                       m_traceRasterSamplesMs);
                        appendDuration(presented->rasterReadyNs,
                                       presented->composeStartNs,
                                       m_traceComposeQueueSamplesMs);
                        appendDuration(presented->composeStartNs,
                                       presented->timestampNs,
                                       m_traceComposeSubmitSamplesMs);
                        appendDuration(presented->clientFrameStartNs,
                                       presented->timestampNs,
                                       m_traceEndToEndSubmitSamplesMs);
                    }
                    if (m_pendingManagedWindowAction &&
                        presented->frameSerial >=
                            m_pendingManagedActionAfterFrameSerial) {
                        const auto action = *m_pendingManagedWindowAction;
                        if (sendProtocolMessage(
                                lcl::protocol::LCLOpcode::RequestManagedWindowAction,
                                &action, sizeof(action))) {
                            m_pendingManagedWindowAction.reset();
                            m_pendingManagedActionAfterFrameSerial = 0;
                        }
                    }
                }
            } else if (header.opcode == lcl::protocol::LCLOpcode::FrameDiscarded &&
                       payload.size() == sizeof(lcl::protocol::LCLMsgFrameDiscarded)) {
                const auto* discarded = reinterpret_cast<
                    const lcl::protocol::LCLMsgFrameDiscarded*>(payload.data());
                if (discarded->surfaceId == m_surfaceId &&
                    discarded->frameSerial == m_submittedFrameSerial) {
                    // A newer configure overtook this raster frame. It can never
                    // be presented, so restore the single-frame credit and
                    // repaint using the newest configure serial.
                    m_submittedConfigureSerial = 0;
                    m_submittedFrameSerial = 0;
                    m_submittedGeometryGeneration = 0;
                    m_retainedRasterFrameSerial = 0;
                    m_scrollTransformFastPathReady = false;
                    m_retainedPresentationFastPathReady = false;
                    m_externalBufferFastPathReady = false;
                    m_frameGateOpen = true;
                    m_firstFrame = true;
                }
            } else if (header.opcode == lcl::protocol::LCLOpcode::AckResponse &&
                       payload.size() == sizeof(lcl::protocol::LCLMsgAckResponse)) {
                const auto* ack = reinterpret_cast<const lcl::protocol::LCLMsgAckResponse*>(payload.data());
                if (ack->status != 0) {
                    std::cerr << "[lcl-ui ERROR] Compositor rejected request "
                              << header.requestId << " for " << m_title
                              << " (status " << ack->status << "): "
                              << ack->message << "\n";
                }
            } else if (header.opcode ==
                           lcl::protocol::LCLOpcode::LaunchIconVisibility &&
                       payload.size() == sizeof(
                           lcl::protocol::LCLMsgLaunchIconVisibility)) {
                const auto* visibility = reinterpret_cast<const
                    lcl::protocol::LCLMsgLaunchIconVisibility*>(payload.data());
                if (visibility->visible != 0) {
                    lcl::protocol::LCLMsgLaunchIconVisibilityAck ack{};
                    ack.launchToken = visibility->launchToken;
                    std::strncpy(ack.appId, visibility->appId,
                                 sizeof(ack.appId) - 1);
                    m_pendingLaunchIconVisibilityAck = ack;
                }
            } else if (header.opcode == lcl::protocol::LCLOpcode::SurfaceDestroy) {
                if (payload.size() >= sizeof(lcl::protocol::LCLMsgSurfaceDestroy)) {
                    auto* destroy = reinterpret_cast<const lcl::protocol::LCLMsgSurfaceDestroy*>(payload.data());
                    if (destroy->surfaceId == m_surfaceId) {
                        m_dispatcher.cancelPointerCaptures();
                        m_running = false;
                        m_surfaceEnded = true;
                    }
                }
            }
            if (receivedFd >= 0 && !receivedFdConsumed) close(receivedFd);
            if (m_onIpcMessage) m_onIpcMessage(header, payload);
        } else if (receiveStatus == lcl::protocol::ReceiveStatus::WouldBlock) {
            break;
        } else {
            std::cerr << "[lcl-ui ERROR] Compositor connection closed or rejected\n";
            m_dispatcher.cancelPointerCaptures();
            m_ipcConnected = false;
            m_running = false;
            m_surfaceEnded = true;
            break;
        }
    }

    if (pendingResize && (latestWidth > 0 && latestHeight > 0)) {
        m_backingWidth = std::max(latestWidth, latestBackingWidth);
        m_backingHeight = std::max(latestHeight, latestBackingHeight);
        m_geometryGeneration = latestGeometryGeneration;
        m_atomicResizeFramePacing =
            latestResizeReason == lcl::protocol::LCLConfigureResizeReason::Interactive ||
            latestResizeReason == lcl::protocol::LCLConfigureResizeReason::WindowStateTransition;

        latestScale = sanitizeBufferScale(latestScale);
        if (std::fabs(latestScale - m_bufferScale) > 0.0001f) {
            m_bufferScale = latestScale;
            updateCanvasRenderTarget();
            // A scale-only configure has identical logical bounds but needs a new buffer.
            if (latestWidth == m_width && latestHeight == m_height && m_ipcConnected) {
                updateCanvasRenderTarget();
                m_firstFrame = true;
            }
        }
        m_pendingResizeWidth = latestWidth;
        m_pendingResizeHeight = latestHeight;
        m_hasPendingResize = true;
    }

    // Throttled resize apply: demos update every pixel while dragging; applying each
    // configure causes SHM recreate storms. Keep latest target and apply at a bounded rate.
    if (m_hasPendingResize && (!m_atomicResizeFramePacing || m_frameGateOpen) &&
        (m_pendingResizeWidth != m_width || m_pendingResizeHeight != m_height)) {
        const auto now = std::chrono::steady_clock::now();
        constexpr auto kMinResizeInterval = std::chrono::milliseconds(22);

        const float dx = std::fabs(m_pendingResizeWidth - m_width);
        const float dy = std::fabs(m_pendingResizeHeight - m_height);

        const bool largeJump = (dx >= 48.0f) || (dy >= 48.0f);
        const bool intervalElapsed = (now - m_lastResizeApply) >= kMinResizeInterval;

        // The first configure establishes the only serial eligible for an
        // initial commit. It must never wait for interactive-resize throttling,
        // even when the compositor adjusted the requested bounds by a few px.
        if (m_atomicResizeFramePacing || receivedInitialConfigure || largeJump || intervalElapsed) {
            m_configureSerial = m_pendingConfigureSerial;
            resize(m_pendingResizeWidth, m_pendingResizeHeight);
            m_waitingForInitialConfigure = false;
            if (m_frameTraceEnabled) ++m_traceResizeApplies;
            m_hasPendingResize = false;
            m_lastResizeApply = now;
        }
    } else if (m_hasPendingResize && (!m_atomicResizeFramePacing || m_frameGateOpen)) {
        // The compositor often confirms the initial logical size verbatim.
        // It requires no SHM reallocation, but it still needs one commit to
        // acknowledge the configure serial and release compositor backpressure.
        m_hasPendingResize = false;
        m_configureSerial = m_pendingConfigureSerial;
        m_waitingForInitialConfigure = false;
        m_firstFrame = true;
    }
}

void WindowApp::runEventLoop() {
    m_running = true;

    while (m_running && !g_appSignalReceived.load()) {
        auto frameStart = std::chrono::high_resolution_clock::now();

        bool rendered = tick();

        if (rendered && !m_atomicResizeFramePacing) {
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::high_resolution_clock::now() - frameStart);
            // Keep app-side pacing close to terminal loop to avoid resize thrash.
            constexpr auto minFramePeriod = std::chrono::microseconds(6900);
            if (elapsed < minFramePeriod) {
                std::this_thread::sleep_for(minFramePeriod - elapsed);
            }
        } else {
            // Idle state: sleep 2ms when no redraws are needed to avoid CPU busy spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    m_running = false;
    if (m_socketFd >= 0 && m_ownsSocketFd) {
        lcl::protocol::discardPendingWrites(m_socketFd);
        close(m_socketFd);
        m_socketFd = -1;
    }
}

bool WindowApp::tick() {
    m_transients.pruneExpiredOwners();
    pollIPC();
    if (m_onFrame) {
        m_onFrame();
    }
    const auto now = std::chrono::steady_clock::now();
    // Procedural presentations advance only when this surface owns a frame
    // credit. While rasterd/compositor still retain an unpresented frame,
    // preserve elapsed time but do not churn widget revisions every 2 ms.
    // The first tick after FramePresented samples the accumulated interval and
    // produces exactly one newest presentation state.
    if (!m_ipcConnected || m_frameGateOpen) {
        const float dtSec = std::chrono::duration<float>(
            now - m_lastAnimationTick).count();
        m_lastAnimationTick = now;
        advanceAnimations(dtSec);
    }
    bool rendered = renderFrame();
    m_tickingHostedSurfaces = true;
    for (auto& entry : m_hostedSurfaces) {
        if (!entry.pendingRemoval && entry.surface) {
            rendered = entry.surface->tick() || rendered;
            if (entry.surface->m_surfaceEnded) entry.pendingRemoval = true;
        }
    }
    m_tickingHostedSurfaces = false;
    collectClosedHostedSurfaces();
    logFrameTraceIfDue();
    return rendered;
}

void WindowApp::collectClosedHostedSurfaces() {
    for (size_t index = 0; index < m_hostedSurfaces.size();) {
        if (!m_hostedSurfaces[index].pendingRemoval) {
            ++index;
            continue;
        }
        auto surface = std::move(m_hostedSurfaces[index].surface);
        auto onClosed = std::move(m_hostedSurfaces[index].onClosed);
        m_hostedSurfaces.erase(m_hostedSurfaces.begin() +
                               static_cast<std::ptrdiff_t>(index));
        surface.reset();
        if (onClosed) onClosed();
    }
}

namespace {
void collectWidgetBounds(Widget* widget, std::unordered_map<uint64_t, graphics::RectF>& bounds) {
    if (!widget) return;
    bounds[widget->getObjectId()] = widget->getAbsoluteBounds();
    for (const auto& child : widget->getChildren()) collectWidgetBounds(child.get(), bounds);
}

void collectVisibleWidgetPaintBounds(
        Widget* widget,
        std::unordered_map<uint64_t, graphics::RectF>& bounds) {
    if (!widget || !widget->isVisible()) return;
    bounds[widget->getObjectId()] = widget->getVisiblePresentationPaintBounds();
    for (const auto& child : widget->getChildren()) {
        collectVisibleWidgetPaintBounds(child.get(), bounds);
    }
}

bool layoutPaintBoundsChanged(const graphics::RectF& before,
                              const graphics::RectF& after) {
    constexpr float epsilon = 0.001f;
    return std::fabs(before.x - after.x) > epsilon ||
        std::fabs(before.y - after.y) > epsilon ||
        std::fabs(before.width - after.width) > epsilon ||
        std::fabs(before.height - after.height) > epsilon;
}

void addChangedLayoutPaintDamage(
        Widget* widget,
        const std::unordered_map<uint64_t, graphics::RectF>& oldBounds,
        RenderPass& renderPass) {
    if (!widget || !widget->isVisible()) return;
    const graphics::RectF current = widget->getVisiblePresentationPaintBounds();
    const auto found = oldBounds.find(widget->getObjectId());
    if (found == oldBounds.end()) {
        renderPass.addDirtyRect(current);
    } else if (layoutPaintBoundsChanged(found->second, current)) {
        renderPass.addDirtyRect(found->second);
        renderPass.addDirtyRect(current);
    }
    for (const auto& child : widget->getChildren()) {
        addChangedLayoutPaintDamage(child.get(), oldBounds, renderPass);
    }
}

void startMorphs(Widget* widget, const std::unordered_map<uint64_t, graphics::RectF>& oldBounds,
                 MotionCoordinator& coordinator, const lcl::motion::Motion& motion,
                 bool& anyMorph) {
    if (!widget) return;
    const auto found = oldBounds.find(widget->getObjectId());
    if (found != oldBounds.end()) {
        const graphics::RectF before = found->second;
        const graphics::RectF after = widget->getAbsoluteBounds();
        if (!before.isEmpty() && !after.isEmpty() &&
            (std::fabs(before.x - after.x) > 0.01f || std::fabs(before.y - after.y) > 0.01f ||
             std::fabs(before.width - after.width) > 0.01f || std::fabs(before.height - after.height) > 0.01f)) {
            anyMorph = true;
            const PresentationState finalPresentation = widget->getPresentationState();
            const float startX = finalPresentation.translationX + before.x - after.x;
            const float startY = finalPresentation.translationY + before.y - after.y;
            const float startScaleX = finalPresentation.scaleX *
                before.width / std::max(0.001f, after.width);
            const float startScaleY = finalPresentation.scaleY *
                before.height / std::max(0.001f, after.height);
            widget->applyPresentationValue(AnimatableProperty::TranslationX, startX);
            widget->applyPresentationValue(AnimatableProperty::TranslationY, startY);
            widget->applyPresentationValue(AnimatableProperty::ScaleX, startScaleX);
            widget->applyPresentationValue(AnimatableProperty::ScaleY, startScaleY);
            coordinator.animateFloat(*widget, AnimatableProperty::TranslationX, startX,
                finalPresentation.translationX, motion,
                [widget](float value) { widget->applyPresentationValue(AnimatableProperty::TranslationX, value); });
            coordinator.animateFloat(*widget, AnimatableProperty::TranslationY, startY,
                finalPresentation.translationY, motion,
                [widget](float value) { widget->applyPresentationValue(AnimatableProperty::TranslationY, value); });
            coordinator.animateFloat(*widget, AnimatableProperty::ScaleX, startScaleX,
                finalPresentation.scaleX, motion,
                [widget](float value) { widget->applyPresentationValue(AnimatableProperty::ScaleX, value); });
            coordinator.animateFloat(*widget, AnimatableProperty::ScaleY, startScaleY,
                finalPresentation.scaleY, motion,
                [widget](float value) { widget->applyPresentationValue(AnimatableProperty::ScaleY, value); });
        }
    }
    for (const auto& child : widget->getChildren()) startMorphs(child.get(), oldBounds, coordinator, motion, anyMorph);
}
} // namespace

void WindowApp::animate(const lcl::motion::Motion& motion,
                        AnimationTransactionOptions options,
                        const std::function<void()>& changes) {
    if (!changes) return;
    std::unordered_map<uint64_t, graphics::RectF> oldBounds;
    if (options.layout == LayoutMode::Morph) {
        collectWidgetBounds(m_windowRoot.get(), oldBounds);
    }
    m_motionCoordinator.beginTransaction(motion, options);
    try {
        changes();
    } catch (...) {
        m_motionCoordinator.endTransaction();
        throw;
    }
    m_motionCoordinator.endTransaction();
    if (options.layout == LayoutMode::Morph) {
        updateLayout();
        bool anyMorph = false;
        startMorphs(m_windowRoot.get(), oldBounds, m_motionCoordinator, motion, anyMorph);
        m_morphInputFrozen = anyMorph;
        if (m_morphInputFrozen) m_dispatcher.cancelPointerCaptures();
    }
}

bool WindowApp::advanceAnimations(float dtSec) {
    const bool motionActive = m_motionCoordinator.tick(dtSec);
    if (m_morphInputFrozen && !motionActive) m_morphInputFrozen = false;
    return motionActive;
}

bool WindowApp::hasActiveAnimations() const noexcept {
    return m_motionCoordinator.hasActiveAnimations();
}

void WindowApp::setExternalIpcSocket(int socketFd) {
    if (socketFd < 0) return;
    m_socketFd = socketFd;
    m_ipcConnected = true;
    m_surfaceEnded = false;
    m_ownsSocketFd = false;
    m_uploadedImageRevisions.clear();
    m_uploadedExternalBuffers.clear();
}

bool WindowApp::requestWindowAction(lcl::protocol::LCLWindowAction action,
                                    float localX, float localY) {
    if (!m_ipcConnected || m_socketFd < 0 || isPopupSurface() ||
        isAttachedSurface()) return false;

    lcl::protocol::LCLMsgRequestWindowAction msg{};
    msg.surfaceId = m_surfaceId;
    msg.action = action;
    msg.localX = localX;
    msg.localY = localY;

    return sendProtocolMessage(lcl::protocol::LCLOpcode::RequestWindowAction,
                               &msg, sizeof(msg));
}

bool WindowApp::requestManagedWindowAction(
        lcl::protocol::LCLWindowAction action,
        float localX, float localY) {
    if (!m_ipcConnected || m_socketFd < 0 || !isAttachedSurface()) return false;

    lcl::protocol::LCLMsgRequestManagedWindowAction msg{};
    msg.targetWindowId = m_attachedWindowId;
    msg.action = action;
    msg.localX = localX;
    msg.localY = localY;
    if (action != lcl::protocol::LCLWindowAction::BeginDrag) {
        m_pendingManagedWindowAction = msg;
        m_pendingManagedActionAfterFrameSerial = m_nextFrameSerial;
        m_firstFrame = true;
        return true;
    }
    return sendProtocolMessage(
        lcl::protocol::LCLOpcode::RequestManagedWindowAction,
        &msg, sizeof(msg));
}

bool WindowApp::requestSurfaceDestroy(uint32_t surfaceId) {
    if (!m_ipcConnected || m_socketFd < 0 || surfaceId == 0) return false;
    lcl::protocol::LCLMsgRequestSurfaceClose close{};
    close.surfaceId = surfaceId;
    return sendProtocolMessage(lcl::protocol::LCLOpcode::RequestSurfaceClose,
                               &close, sizeof(close));
}

bool WindowApp::sendProtocolMessage(lcl::protocol::LCLOpcode opcode, const void* payload,
                                    uint32_t payloadSize, int passedFd) {
    if (m_socketFd < 0) return false;
    lcl::protocol::LCLHeader header{};
    header.opcode = opcode;
    header.requestId = m_nextRequestId++;
    if (m_nextRequestId == 0) m_nextRequestId = 1;
    header.payloadSize = payloadSize;
    return lcl::protocol::sendMsgWithFd(m_socketFd, header, payload, passedFd);
}

bool WindowApp::uploadImageResource(const graphics::ImageResourceView& resource) {
    if (resource.id == 0 || resource.contentRevision == 0 ||
        resource.width == 0 || resource.height == 0 ||
        resource.stridePixels < resource.width || !resource.pixels) {
        return false;
    }
    if (const auto found = m_uploadedImageRevisions.find(resource.id);
        found != m_uploadedImageRevisions.end() &&
        found->second == resource.contentRevision) {
        return true;
    }

    const bool sent = m_rasterClient->uploadImage(resource);
    if (sent) m_uploadedImageRevisions[resource.id] = resource.contentRevision;
    return sent;
}

bool WindowApp::uploadExternalBufferResources(const Widget& widget) {
    if (!widget.isVisible()) return true;
    if (const auto* external = dynamic_cast<const ExternalBufferView*>(&widget)) {
        if (const auto* stored = external->storedFrame()) {
            const auto& descriptor = stored->descriptor;
            const auto found = m_uploadedExternalBuffers.find(
                descriptor.bufferId);
            if (found == m_uploadedExternalBuffers.end() ||
                found->second.contentRevision != descriptor.contentRevision) {
                if (!m_rasterClient->uploadExternalBuffer(descriptor)) {
                    return false;
                }
                m_uploadedExternalBuffers[descriptor.bufferId] = {
                    descriptor.contentRevision,
                    [release = stored->release](int fenceFd) {
                        if (release->releaseFenceFd >= 0) {
                            close(release->releaseFenceFd);
                        }
                        release->releaseFenceFd = fenceFd;
                    }};
            }
        }
    }
    for (const auto& child : widget.getChildren()) {
        if (child && !uploadExternalBufferResources(*child)) return false;
    }
    return true;
}

bool WindowApp::requestWindowDrag(float localX, float localY) {
    return requestWindowAction(lcl::protocol::LCLWindowAction::BeginDrag, localX, localY);
}

bool WindowApp::requestWindowMinimize() {
    return requestWindowAction(lcl::protocol::LCLWindowAction::Minimize);
}

bool WindowApp::requestWindowMaximize() {
    return requestWindowAction(lcl::protocol::LCLWindowAction::Maximize);
}

bool WindowApp::requestWindowRestore() {
    return requestWindowAction(lcl::protocol::LCLWindowAction::Restore);
}

bool WindowApp::requestWindowToggleMaximize() {
    return requestWindowAction(lcl::protocol::LCLWindowAction::ToggleMaximize);
}

bool WindowApp::requestWindowClose() {
    if (isPopupSurface() || isAttachedSurface()) {
        return requestSurfaceDestroy(m_surfaceId);
    }
    return requestWindowAction(lcl::protocol::LCLWindowAction::Close);
}

bool WindowApp::beginLaunchPlaceholder(
        uint64_t launchToken, const std::string& appId,
        const graphics::RectF& origin, float cornerRadius,
        uint32_t iconWidth, uint32_t iconHeight,
        const std::vector<uint32_t>& iconPixels) {
    constexpr uint64_t kLaunchTokenLimit = uint64_t{1} << 63;
    const size_t iconPixelCount = static_cast<size_t>(iconWidth) * iconHeight;
    if (!m_ipcConnected || m_socketFd < 0 ||
        m_systemSurfaceKind != lcl::protocol::LCLSystemSurfaceKind::HomeScreen ||
        launchToken == 0 || launchToken >= kLaunchTokenLimit ||
        appId.empty() || origin.isEmpty() ||
        cornerRadius < 0.0f || iconWidth == 0 || iconHeight == 0 ||
        iconWidth > lcl::protocol::LCL_LAUNCH_ICON_MAX_DIMENSION ||
        iconHeight > lcl::protocol::LCL_LAUNCH_ICON_MAX_DIMENSION ||
        iconPixels.size() != iconPixelCount) return false;
    lcl::protocol::LCLMsgBeginLaunchPlaceholder message{};
    message.homeSurfaceId = m_surfaceId;
    message.launchToken = launchToken;
    std::strncpy(message.appId, appId.c_str(), sizeof(message.appId) - 1);
    message.originX = origin.x;
    message.originY = origin.y;
    message.originWidth = origin.width;
    message.originHeight = origin.height;
    message.originCornerRadius = cornerRadius;
    message.iconWidth = iconWidth;
    message.iconHeight = iconHeight;
    std::vector<uint8_t> payload(
        sizeof(message) + iconPixelCount * sizeof(uint32_t));
    std::memcpy(payload.data(), &message, sizeof(message));
    std::memcpy(payload.data() + sizeof(message), iconPixels.data(),
                iconPixelCount * sizeof(uint32_t));
    return sendProtocolMessage(
        lcl::protocol::LCLOpcode::BeginLaunchPlaceholder,
        payload.data(), static_cast<uint32_t>(payload.size()));
}

bool WindowApp::resolveLaunchPlaceholder(
        uint64_t launchToken, uint64_t appInstanceId, bool reused) {
    constexpr uint64_t kLaunchTokenLimit = uint64_t{1} << 63;
    if (!m_ipcConnected || m_socketFd < 0 ||
        m_systemSurfaceKind != lcl::protocol::LCLSystemSurfaceKind::HomeScreen ||
        launchToken == 0 || launchToken >= kLaunchTokenLimit ||
        appInstanceId == 0) return false;
    lcl::protocol::LCLMsgResolveLaunchPlaceholder message{};
    message.homeSurfaceId = m_surfaceId;
    message.launchToken = launchToken;
    message.appInstanceId = appInstanceId;
    message.reused = reused ? 1 : 0;
    return sendProtocolMessage(
        lcl::protocol::LCLOpcode::ResolveLaunchPlaceholder,
        &message, sizeof(message));
}

bool WindowApp::cancelLaunchPlaceholder(uint64_t launchToken) {
    constexpr uint64_t kLaunchTokenLimit = uint64_t{1} << 63;
    if (!m_ipcConnected || m_socketFd < 0 ||
        m_systemSurfaceKind != lcl::protocol::LCLSystemSurfaceKind::HomeScreen ||
        launchToken == 0 || launchToken >= kLaunchTokenLimit) return false;
    lcl::protocol::LCLMsgCancelLaunchPlaceholder message{};
    message.homeSurfaceId = m_surfaceId;
    message.launchToken = launchToken;
    return sendProtocolMessage(
        lcl::protocol::LCLOpcode::CancelLaunchPlaceholder,
        &message, sizeof(message));
}

bool WindowApp::setDecorationMode(lcl::protocol::LCLDecorationMode mode) {
    m_requestedDecorationMode = mode;
    m_hasRequestedDecorationMode = true;
    if (!m_ipcConnected || m_socketFd < 0) return true;

    lcl::protocol::LCLMsgSetDecorationMode msg{};
    msg.surfaceId = m_surfaceId;
    msg.mode = mode;

    return sendProtocolMessage(lcl::protocol::LCLOpcode::SetDecorationMode,
                               &msg, sizeof(msg));
}

bool WindowApp::setEdgeToEdge(bool enabled) {
    m_requestedEdgeToEdge = enabled;
    m_hasRequestedEdgeToEdge = true;
    if (!m_ipcConnected || m_socketFd < 0) return true;

    lcl::protocol::LCLMsgSetEdgeToEdge msg{};
    msg.surfaceId = m_surfaceId;
    msg.enabled = enabled ? 1 : 0;
    return sendProtocolMessage(lcl::protocol::LCLOpcode::SetEdgeToEdge,
                               &msg, sizeof(msg));
}

bool WindowApp::setWindowLayer(lcl::protocol::LCLWindowLayer layer, bool unfocusable) {
    if (!m_ipcConnected || m_socketFd < 0) return false;

    lcl::protocol::LCLMsgSetWindowLayer msg{};
    msg.surfaceId = m_surfaceId;
    msg.layer = layer;
    msg.unfocusable = unfocusable ? 1 : 0;
    return sendProtocolMessage(lcl::protocol::LCLOpcode::SetWindowLayer,
                               &msg, sizeof(msg));
}

bool WindowApp::setReservedZone(float top, float bottom, float left, float right) {
    if (!m_ipcConnected || m_socketFd < 0) return false;

    lcl::protocol::LCLMsgSetReservedZone msg{};
    msg.surfaceId = m_surfaceId;
    msg.top = top;
    msg.bottom = bottom;
    msg.left = left;
    msg.right = right;
    return sendProtocolMessage(lcl::protocol::LCLOpcode::SetReservedZone,
                               &msg, sizeof(msg));
}

bool WindowApp::setWindowCornerStyle(float radius, float roundness) {
    m_requestedCornerRadius = std::max(0.0f, radius);
    m_requestedCornerRoundness = std::clamp(roundness, 2.0f, 8.0f);
    m_hasRequestedCornerRadius = true;
    if (!m_ipcConnected || m_socketFd < 0) return true;

    lcl::protocol::LCLMsgSetWindowCornerStyle msg{};
    msg.surfaceId = m_surfaceId;
    msg.radius = m_requestedCornerRadius;
    msg.roundness = m_requestedCornerRoundness;

    return sendProtocolMessage(lcl::protocol::LCLOpcode::SetWindowCornerStyle,
                               &msg, sizeof(msg));
}

bool WindowApp::setWindowCornerRadius(float radius) {
    return setWindowCornerStyle(radius, m_requestedCornerRoundness);
}

bool WindowApp::sendPointerMove(float x, float y, PointerSource source, uint32_t pointerId) {
    if (m_morphInputFrozen) return false;
    PointerEvent ev{x, y, 0, 0.0f, 0.0f, PointerEventType::Move, source, pointerId};
    if (!m_dispatcher.hasPointerCapture(pointerId) &&
        m_onRawPointer && m_onRawPointer(ev)) {
        return true;
    }
    return m_dispatcher.dispatchPointerEvent(m_windowRoot.get(), &m_transients, ev);
}

bool WindowApp::sendPointerDown(float x, float y, int button, PointerSource source,
                                uint32_t pointerId) {
    if (m_morphInputFrozen) return false;
    PointerEvent ev{x, y, button, 0.0f, 0.0f, PointerEventType::Down, source, pointerId};
    if (m_onRawPointer && m_onRawPointer(ev)) {
        return true;
    }
    return m_dispatcher.dispatchPointerEvent(m_windowRoot.get(), &m_transients, ev);
}

bool WindowApp::sendPointerUp(float x, float y, int button, PointerSource source,
                              uint32_t pointerId) {
    if (m_morphInputFrozen) return false;
    PointerEvent ev{x, y, button, 0.0f, 0.0f, PointerEventType::Up, source, pointerId};
    if (!m_dispatcher.hasPointerCapture(pointerId) &&
        m_onRawPointer && m_onRawPointer(ev)) {
        return true;
    }
    return m_dispatcher.dispatchPointerEvent(m_windowRoot.get(), &m_transients, ev);
}

bool WindowApp::sendPointerCancel(float x, float y, PointerSource source,
                                  uint32_t pointerId) {
    PointerEvent ev{x, y, 0, 0.0f, 0.0f, PointerEventType::Cancel, source, pointerId};
    if (!m_dispatcher.hasPointerCapture(pointerId) &&
        m_onRawPointer && m_onRawPointer(ev)) {
        return true;
    }
    return m_dispatcher.dispatchPointerEvent(m_windowRoot.get(), &m_transients, ev);
}

bool WindowApp::sendPointerScroll(float x, float y, float deltaX, float deltaY,
                                  PointerSource source, uint32_t pointerId) {
    if (m_morphInputFrozen) return false;
    PointerEvent ev{x, y, 0, deltaX, deltaY, PointerEventType::Scroll, source, pointerId};
    if (m_onRawPointer && m_onRawPointer(ev)) {
        return true;
    }
    return m_dispatcher.dispatchPointerEvent(m_windowRoot.get(), &m_transients, ev);
}

bool WindowApp::sendKeyDown(int keyCode, char32_t codepoint, uint8_t modifiers) {
    KeyEvent ev{static_cast<lcl::platform::PhysicalKey>(keyCode), keyCode, codepoint, modifiers, KeyEventType::KeyDown};
    if (m_onRawKey && m_onRawKey(ev)) {
        return true;
    }
    return m_dispatcher.dispatchKeyEvent(m_windowRoot.get(), ev);
}

bool WindowApp::sendKeyUp(int keyCode, uint8_t modifiers) {
    KeyEvent ev{static_cast<lcl::platform::PhysicalKey>(keyCode), keyCode, 0, modifiers, KeyEventType::KeyUp};
    if (m_onRawKey && m_onRawKey(ev)) {
        return true;
    }
    return m_dispatcher.dispatchKeyEvent(m_windowRoot.get(), ev);
}

bool WindowApp::sendTextInput(const std::string& text) {
    TextInputEvent ev{text};
    if (m_onRawTextInput && m_onRawTextInput(ev)) {
        return true;
    }
    return m_dispatcher.dispatchTextInputEvent(ev);
}

void WindowApp::updateLayout() {
    if (m_windowRoot) {
        const auto started = std::chrono::steady_clock::now();
        // calculateLayout() services the current request first. A widget that
        // performs a genuine layout mutation from measurement or syncLayout()
        // starts a new epoch and receives another pass on the next frame.
        m_windowRoot->calculateLayout(static_cast<float>(m_width), static_cast<float>(m_height));
        m_windowRoot->syncLayout(0.0f, 0.0f);
        if (m_frameTraceEnabled) {
            ++m_traceLayoutPasses;
            m_traceLayoutMs += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
        }
    }
}

bool WindowApp::renderFrame() {
    // The compositor owns configure serial assignment. A buffer rendered from
    // SurfaceCreate's requested geometry has no valid serial and would be
    // discarded as stale when the initial ConfigureBounds is already queued.
    if (m_ipcConnected && m_waitingForInitialConfigure) return false;
    if (m_ipcConnected && !m_frameGateOpen) return false;

    const uint64_t traceFrameStartNs =
        m_frameTraceEnabled ? monotonicNowNs() : 0;

    if (m_ipcConnected && m_canvas->usesDisplayListTransport()) {
        if (!m_rasterClient->prepare()) {
            m_firstFrame = true;
            return false;
        }
        const uint64_t connectionGeneration =
            m_rasterClient->connectionGeneration();
        if (connectionGeneration != m_rasterConnectionGeneration) {
            // A fresh daemon has no retained scene or uploaded resources.
            m_uploadedImageRevisions.clear();
            m_uploadedExternalBuffers.clear();
            m_retainedRasterFrameSerial = 0;
            m_scrollTransformFastPathReady = false;
            m_retainedPresentationFastPathReady = false;
            m_externalBufferFastPathReady = false;
            m_rasterConnectionGeneration = connectionGeneration;
            m_firstFrame = true;
        }
    }

    if (m_windowRoot && m_windowRoot->isLayoutDirty()) {
        std::unordered_map<uint64_t, graphics::RectF> oldPaintBounds;
        collectVisibleWidgetPaintBounds(m_windowRoot.get(), oldPaintBounds);
        updateLayout();
        // Layout can move several siblings without each new bound producing a
        // paint invalidation. Damage the exact old/new visible extents instead
        // of turning a dynamic Text measurement into a full-surface redraw.
        addChangedLayoutPaintDamage(
            m_windowRoot.get(), oldPaintBounds, m_renderPass);
    }

    const graphics::RectF surfaceBounds{
        0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height)};

    // A resize can leave old-layout damage queued before layout computes the
    // new child positions. Always include the complete post-layout root extent
    // on the first frame so children moved outside the old damage are painted.
    if (m_firstFrame) {
        m_renderPass.addDirtyRect(surfaceBounds);
    }
    if (!m_renderPass.hasDamage()) return false;
    const bool replacesRetainedScene = m_firstFrame;
    m_firstFrame = false;
    if (replacesRetainedScene && m_windowRoot) {
        // Rasterd restart, resize, rejection and root replacement all discard
        // server-owned cached textures. Force cache-aware widgets to publish
        // their logical source template in the replacement transaction.
        invalidateRetainedWidgetCaches(*m_windowRoot);
    }

    // Path coverage can extend one physical pixel past its analytic bounds.
    // Keep widget damage logical and add that backend sampling margin only at
    // the raster boundary, before clear, paint clipping and buffer copy.
    const float deviceScale = sanitizeBufferScale(
        m_canvas->renderTarget().deviceScale);
    const float rasterOutset = 1.0f / deviceScale;
    RenderPass rasterDamage;
    for (const graphics::RectF& dirty : m_renderPass.getDirtyRects()) {
        const graphics::RectF expanded{
            dirty.x - rasterOutset,
            dirty.y - rasterOutset,
            dirty.width + rasterOutset * 2.0f,
            dirty.height + rasterOutset * 2.0f,
        };
        const graphics::RectF clipped = expanded.intersection(surfaceBounds);
        if (!clipped.isEmpty()) rasterDamage.addDirtyRect(clipped);
    }
    RenderPass contentRasterDamage;
    for (const graphics::RectF& dirty :
         m_renderPass.getRasterDirtyRects()) {
        const graphics::RectF expanded{
            dirty.x - rasterOutset,
            dirty.y - rasterOutset,
            dirty.width + rasterOutset * 2.0f,
            dirty.height + rasterOutset * 2.0f,
        };
        const graphics::RectF clipped = expanded.intersection(surfaceBounds);
        if (!clipped.isEmpty()) contentRasterDamage.addDirtyRect(clipped);
    }
    std::vector<graphics::RectF> damageRects = rasterDamage.getDirtyRects();
    std::vector<graphics::RectF> contentDamageRects =
        contentRasterDamage.getDirtyRects();
    m_renderPass.clear();
    if (damageRects.empty()) return false;
    graphics::RectF frameDamage = rasterDamage.getDamageRect();

    // Compile the exact post-layout Widget state that this frame will paint.
    // The snapshot is private and does not alter DisplayList recording or IPC;
    // the next retained-protocol phase will translate it into transactions.
    if (m_windowRoot) {
        if (m_frameTraceEnabled) {
            const auto renderTreeStarted = std::chrono::steady_clock::now();
            m_privateRenderState->synchronize(
                *m_windowRoot, replacesRetainedScene);
            m_traceRenderCreates +=
                m_privateRenderState->createdNodeCount();
            m_traceRenderContentUpdates +=
                m_privateRenderState->contentUpdateCount();
            m_traceRenderPropertyUpdates +=
                m_privateRenderState->propertyUpdateCount();
            m_traceRenderRemovals +=
                m_privateRenderState->removedNodeCount();
            m_traceRenderTreeMs += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - renderTreeStarted).count();
        } else {
            m_privateRenderState->synchronize(
                *m_windowRoot, replacesRetainedScene);
        }
        configureRetainedWidgetCaches(*m_windowRoot);
    }

    const bool scrollTransformTransaction =
        m_privateRenderState->isScrollTransformOnly();
    const bool scrollTransformOnly =
        m_scrollTransformFastPathReady && scrollTransformTransaction;
    const bool retainedPresentationTransaction =
        m_privateRenderState->isRetainedPresentationOnly();
    const bool retainedPresentationOnly =
        m_retainedPresentationFastPathReady &&
        retainedPresentationTransaction;
    const bool retainedPresentationRecovery =
        !m_retainedPresentationFastPathReady &&
        retainedPresentationTransaction;
    const bool externalBufferTransaction =
        m_privateRenderState->isExternalBufferOnly();
    const bool externalBufferOnly =
        m_externalBufferFastPathReady && externalBufferTransaction;
    if (retainedPresentationRecovery) {
        // A recovered composition template must cover future transform
        // extents, not only the first old/new animation bounds. One complete
        // scene establishes that stable base; subsequent property commits are
        // DisplayList-free again.
        damageRects.assign(1, surfaceBounds);
        frameDamage = surfaceBounds;
    }
    const bool propertyOnly =
        scrollTransformOnly || retainedPresentationOnly || externalBufferOnly;
    m_canvas->beginFrame();

    const auto paintStarted = std::chrono::steady_clock::now();
    if (m_frameTraceEnabled) {
        ++m_traceRenderedFrames;
        for (const graphics::RectF& damage : damageRects) {
            m_traceDamagePixels +=
                static_cast<uint64_t>(std::max(0.0f, damage.width)) *
                static_cast<uint64_t>(std::max(0.0f, damage.height));
        }
    }

    const auto clearStarted = paintStarted;
    auto drawStarted = clearStarted;
    if (!propertyOnly) {
        for (const graphics::RectF& damage : damageRects) {
            m_canvas->clearRect(damage, {0, 0, 0, 0});
            if (m_frameTraceEnabled) {
                m_traceClearedBytes +=
                    static_cast<uint64_t>(std::ceil(damage.width * m_bufferScale)) *
                    static_cast<uint64_t>(std::ceil(damage.height * m_bufferScale)) *
                    sizeof(uint32_t);
            }
        }
        if (m_frameTraceEnabled) {
            m_traceClearMs += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - clearStarted).count();
        }

        drawStarted = std::chrono::steady_clock::now();
        // Cache-aware widgets need the complete region set for this immutable
        // frame. The root is still traversed once per clip below, but a ScrollView
        // must patch every changed descendant region before acknowledging the
        // content revision seen by those separate traversals.
        m_renderPass.begin(*m_canvas, damageRects, contentDamageRects);

        for (const graphics::RectF& damage : damageRects) {
            m_canvas->saveState();
            m_canvas->clipRect(damage);
            if (m_windowRoot && m_windowRoot->isVisible()) {
                m_windowRoot->draw(*m_canvas, damage);
            }
            if (m_layoutOverlayEnabled && m_windowRoot) {
                drawLayoutOverlay(*m_windowRoot, *m_canvas);
                m_canvas->drawRoundedRect(damage, 0.0f, {0, 0, 0, 0},
                                          {239, 68, 68, 255}, 1.0f, 2.0f);
            }
            m_canvas->restoreState();
        }

        std::vector<EffectRegion> uiEffects;
        if (m_windowRoot) m_windowRoot->collectEffects(uiEffects);

        m_renderPass.end(*m_canvas);
        if (m_ipcConnected && m_socketFd >= 0) {
            auto toProtoSource = [](EffectSource source) {
                switch (source) {
                    case EffectSource::Layer:
                        return lcl::protocol::EffectSourceType::Layer;
                    case EffectSource::SurfaceBackdrop:
                        return lcl::protocol::EffectSourceType::SurfaceBackdrop;
                    case EffectSource::Backdrop:
                        // In-app backdrops are recorded in the DisplayList and
                        // must never be evaluated against the desktop scene.
                        // This fallback is unreachable because Widget excludes
                        // Backdrop from the compositor effect graph.
                        return lcl::protocol::EffectSourceType::Layer;
                }
                return lcl::protocol::EffectSourceType::Layer;
            };

            auto toProtoBlend = [](EffectBlend blend) {
                switch (blend) {
                    case EffectBlend::Screen:   return lcl::protocol::EffectBlendMode::Screen;
                    case EffectBlend::Multiply: return lcl::protocol::EffectBlendMode::Multiply;
                    case EffectBlend::Overlay:  return lcl::protocol::EffectBlendMode::Overlay;
                    case EffectBlend::Plus:     return lcl::protocol::EffectBlendMode::Plus;
                    case EffectBlend::Normal:
                    default:                    return lcl::protocol::EffectBlendMode::Normal;
                }
            };
            auto toProtoBounds = [](EffectBounds bounds) {
                return bounds == EffectBounds::OuterSurface
                    ? lcl::protocol::EffectBoundsPolicy::OuterSurface
                    : lcl::protocol::EffectBoundsPolicy::Local;
            };

            std::vector<lcl::protocol::EffectRegion> protoRegions;
            std::vector<lcl::protocol::FilterOp> flatFilters;
            protoRegions.reserve(uiEffects.size());

            for (const auto& effect : uiEffects) {
                if (effect.bounds.isEmpty() || effect.filters.empty()) continue;

                const float x = std::max(0.0f, effect.bounds.x);
                const float y = std::max(0.0f, effect.bounds.y);
                if (x >= m_width || y >= m_height) continue;
                const float w = std::min(std::max(0.0f, effect.bounds.width), m_width - x);
                const float h = std::min(std::max(0.0f, effect.bounds.height), m_height - y);
                if (w <= 0.0f || h <= 0.0f) continue;

                lcl::protocol::EffectRegion region{};
                region.x = x;
                region.y = y;
                region.width = w;
                region.height = h;
                region.cornerRadius = std::max(0.0f, effect.cornerRadius);
                region.cornerRoundness = std::clamp(effect.cornerRoundness, 2.0f, 8.0f);
                region.boundsPolicy = toProtoBounds(effect.boundsPolicy);
                region.source = toProtoSource(effect.source);
                region.blendMode = toProtoBlend(effect.blend);
                region.opacity = std::clamp(effect.opacity, 0.0f, 1.0f);
                region.filterOffset = static_cast<uint32_t>(flatFilters.size());
                region.filterCount = static_cast<uint16_t>(std::min<size_t>(effect.filters.size(), 65535));

                for (uint16_t i = 0; i < region.filterCount; ++i) {
                    auto filter = effect.filters[i];
                    flatFilters.push_back(filter);
                }
                protoRegions.push_back(region);
            }

            if (!protoRegions.empty()) {
                lcl::protocol::LCLMsgSetEffectGraphHeader graphMsg{};
                graphMsg.surfaceId = m_surfaceId;
                graphMsg.regionCount = static_cast<uint32_t>(protoRegions.size());
                graphMsg.filterCount = static_cast<uint32_t>(flatFilters.size());

                const size_t payloadSize =
                    sizeof(graphMsg) +
                    protoRegions.size() * sizeof(lcl::protocol::EffectRegion) +
                    flatFilters.size() * sizeof(lcl::protocol::FilterOp);

                std::vector<uint8_t> payload(payloadSize);
                uint8_t* dst = payload.data();
                std::memcpy(dst, &graphMsg, sizeof(graphMsg));
                dst += sizeof(graphMsg);
                std::memcpy(dst, protoRegions.data(), protoRegions.size() * sizeof(lcl::protocol::EffectRegion));
                dst += protoRegions.size() * sizeof(lcl::protocol::EffectRegion);
                if (!flatFilters.empty()) {
                    std::memcpy(dst, flatFilters.data(), flatFilters.size() * sizeof(lcl::protocol::FilterOp));
                }

                if (payload != m_lastEffectGraphPayload) {
                    sendProtocolMessage(lcl::protocol::LCLOpcode::SetEffectGraph,
                                        payload.data(), static_cast<uint32_t>(payload.size()));
                    m_lastEffectGraphPayload = std::move(payload);
                }
                m_effectGraphActive = true;
            } else if (m_effectGraphActive) {
                lcl::protocol::LCLMsgClearEffectGraph clearMsg{};
                clearMsg.surfaceId = m_surfaceId;

                sendProtocolMessage(lcl::protocol::LCLOpcode::ClearEffectGraph,
                                    &clearMsg, sizeof(clearMsg));
                m_effectGraphActive = false;
                m_lastEffectGraphPayload.clear();
            }
        }
    }
    m_canvas->endFrame();
    if (m_frameTraceEnabled && !propertyOnly) {
        m_traceDrawMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - drawStarted).count();
    }

    // Connected clients submit the closed logical frame to central rasterd.
    const auto rasterSubmitStarted = std::chrono::steady_clock::now();
    bool frameAttached = false;
    if (m_ipcConnected && m_socketFd >= 0 &&
        m_canvas->usesDisplayListTransport()) {
        if (auto frame = m_canvas->takeDisplayListFrame()) {
            if (!m_rasterClient->prepare()) {
                m_firstFrame = true;
                return false;
            }
            const uint64_t connectionGeneration =
                m_rasterClient->connectionGeneration();
            if (connectionGeneration != m_rasterConnectionGeneration) {
                // The patch was recorded against a daemon that disappeared.
                // Retry as a complete retained-scene replacement.
                m_uploadedImageRevisions.clear();
                m_uploadedExternalBuffers.clear();
                m_retainedRasterFrameSerial = 0;
                m_scrollTransformFastPathReady = false;
                m_retainedPresentationFastPathReady = false;
                m_externalBufferFastPathReady = false;
                m_rasterConnectionGeneration = connectionGeneration;
                m_firstFrame = true;
                return false;
            }
            // A complete replacement defines the active resource set. A patch
            // mentions only resources touched by its damage, so it must not
            // evict upload admission state for unchanged retained content.
            if (replacesRetainedScene) {
                std::erase_if(
                    m_uploadedImageRevisions,
                    [&frame](const auto& uploaded) {
                        return std::none_of(
                            frame->imageResources.begin(),
                            frame->imageResources.end(),
                            [&uploaded](const auto& active) {
                                return active.id == uploaded.first &&
                                    active.contentRevision == uploaded.second;
                            });
                    });
            }
            const bool frameHasRetainedPresentationLayer =
                !propertyOnly && std::any_of(
                    frame->displayList.commands().begin(),
                    frame->displayList.commands().end(),
                    [&](const auto& command) {
                        const auto* draw = std::get_if<
                            graphics::DrawCachedLayerCommand>(&command);
                        return draw && m_privateRenderState->
                            isRetainedPresentationLayer(draw->id);
                    });
            const bool frameHasExternalBufferPlaceholder =
                !propertyOnly && std::any_of(
                    frame->displayList.commands().begin(),
                    frame->displayList.commands().end(),
                    [](const auto& command) {
                        return std::holds_alternative<
                            graphics::DrawExternalBufferCommand>(command);
                    });
            bool resourcesReady = true;
            if (m_windowRoot &&
                !uploadExternalBufferResources(*m_windowRoot)) {
                resourcesReady = false;
            }
            if (!propertyOnly) {
                for (const auto& resource : frame->imageResources) {
                    if (!uploadImageResource(resource)) {
                        resourcesReady = false;
                        break;
                    }
                }
            }

            constexpr size_t kMaxWireBytes = raster_protocol::kMaxPayload;
            auto encoded = resourcesReady && !propertyOnly
                ? graphics::encodeDisplayList(frame->displayList, kMaxWireBytes)
                : graphics::DisplayListEncodeResult{};
            if (resourcesReady && encoded) {
                const uint64_t frameSerial = m_nextFrameSerial++;
                if (m_nextFrameSerial == 0) m_nextFrameSerial = 1;
                if (m_rasterClient->commitTransaction(
                        m_configureSerial, frameSerial,
                        m_retainedRasterFrameSerial, m_geometryGeneration,
                        m_width, m_height, m_bufferScale, frameDamage,
                        m_privateRenderState->currentTransaction(),
                        encoded.bytes, traceFrameStartNs)) {
                    m_submittedConfigureSerial = m_configureSerial;
                    m_submittedFrameSerial = frameSerial;
                    m_submittedGeometryGeneration = m_geometryGeneration;
                    const uint64_t connectionGeneration =
                        m_rasterClient->connectionGeneration();
                    if (m_rasterConnectionGeneration != 0 &&
                        connectionGeneration != m_rasterConnectionGeneration) {
                        // The daemon restarted between resource admission and
                        // frame submission. Retry with a complete resource set.
                        m_uploadedImageRevisions.clear();
                        m_uploadedExternalBuffers.clear();
                        m_externalBufferFastPathReady = false;
                        m_firstFrame = true;
                    }
                    m_rasterConnectionGeneration = connectionGeneration;
                    m_frameGateOpen = false;
                    frameAttached = true;
                    if (scrollTransformOnly && m_frameTraceEnabled) {
                        ++m_traceScrollTransformTransactions;
                    }
                    if (retainedPresentationOnly && m_frameTraceEnabled) {
                        ++m_traceRetainedPresentationTransactions;
                    }
                    if (replacesRetainedScene) {
                        m_scrollTransformFastPathReady =
                            m_privateRenderState->hasScrollContent();
                        m_externalBufferFastPathReady =
                            frameHasExternalBufferPlaceholder;
                    } else if (scrollTransformTransaction) {
                        // If the previous template was invalidated, this
                        // ordinary DisplayList frame repopulates it. Later
                        // touch moves can return to property-only commits.
                        m_scrollTransformFastPathReady = true;
                    } else if (m_privateRenderState->
                                   invalidatesRetainedCompositionTemplate()) {
                        m_scrollTransformFastPathReady = false;
                    }
                    if (!retainedPresentationOnly) {
                        if (!m_privateRenderState->
                                hasRetainedPresentationLayers()) {
                            m_retainedPresentationFastPathReady = false;
                        } else if (replacesRetainedScene ||
                                   retainedPresentationRecovery) {
                            m_retainedPresentationFastPathReady =
                                frameHasRetainedPresentationLayer;
                        } else if (m_privateRenderState->
                                       invalidatesRetainedCompositionTemplate()) {
                            m_retainedPresentationFastPathReady = false;
                        }
                    }
                    if (!externalBufferOnly &&
                        frameHasExternalBufferPlaceholder) {
                        m_externalBufferFastPathReady = true;
                    } else if (!externalBufferOnly &&
                               m_privateRenderState->
                                   invalidatesRetainedCompositionTemplate()) {
                        m_externalBufferFastPathReady = false;
                    }
                }
            }
            if (!frameAttached) {
                m_uploadedImageRevisions.clear();
                m_uploadedExternalBuffers.clear();
                m_retainedRasterFrameSerial = 0;
                m_scrollTransformFastPathReady = false;
                m_retainedPresentationFastPathReady = false;
                m_externalBufferFastPathReady = false;
                m_rasterClient->disconnect();
                m_firstFrame = true;
                std::cerr << "[lcl-ui ERROR] DisplayList commit failed for "
                          << m_title << "; retrying\n";
            }
        } else {
            m_firstFrame = true;
        }
    }
    if (frameAttached && m_pendingLaunchIconVisibilityAck) {
        const auto& ack = *m_pendingLaunchIconVisibilityAck;
        if (sendProtocolMessage(
                lcl::protocol::LCLOpcode::LaunchIconVisibilityAck,
                &ack, sizeof(ack))) {
            m_pendingLaunchIconVisibilityAck.reset();
        }
    }
    if (m_frameTraceEnabled) {
        m_traceRasterSubmitMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - rasterSubmitStarted).count();
    }

    if (m_frameTraceEnabled) {
        m_tracePaintMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - paintStarted).count();
    }
    return true;
}

void WindowApp::invalidateRetainedWidgetCaches(Widget& widget) noexcept {
    widget.invalidateRetainedPresentationCache();
    if (auto* scroll = dynamic_cast<ScrollView*>(&widget)) {
        scroll->invalidateRetainedCache();
    }
    for (const auto& child : widget.getChildren()) {
        if (child) invalidateRetainedWidgetCaches(*child);
    }
}

void WindowApp::configureRetainedWidgetCaches(Widget& widget) noexcept {
    widget.setRetainedPresentationBoundary(
        m_privateRenderState->isRetainedPresentationLayer(
            widget.getObjectId()));
    for (const auto& child : widget.getChildren()) {
        if (child) configureRetainedWidgetCaches(*child);
    }
}

void WindowApp::logFrameTraceIfDue() {
    if (!m_frameTraceEnabled) return;
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - m_traceLastLog).count();
    if (seconds < 1.0) return;
    const auto average = [](double total, uint64_t count) {
        return count == 0 ? 0.0 : total / static_cast<double>(count);
    };
    const auto percentile95 = [](std::vector<double>& samples) {
        if (samples.empty()) return 0.0;
        std::sort(samples.begin(), samples.end());
        const size_t index = (samples.size() * 95 + 99) / 100 - 1;
        return samples[std::min(index, samples.size() - 1)];
    };
    std::cerr << "[LCL TRACE " << m_title << "] frames=" << m_traceRenderedFrames
              << " layout=" << m_traceLayoutPasses << " (" << average(m_traceLayoutMs, m_traceLayoutPasses) << " ms)"
              << " paint=" << average(m_tracePaintMs, m_traceRenderedFrames) << " ms"
              << " stages=(clear=" << average(m_traceClearMs, m_traceRenderedFrames)
              << ", tree=" << average(m_traceRenderTreeMs, m_traceRenderedFrames)
              << ", draw=" << average(m_traceDrawMs, m_traceRenderedFrames)
              << ", raster-submit=" << average(m_traceRasterSubmitMs, m_traceRenderedFrames) << " ms)"
              << " damage=" << m_traceDamagePixels << " px"
              << " clear=" << (m_traceClearedBytes / 1024) << " KiB"
              << " cfg=" << m_traceConfigureCount << " (interactive=" << m_traceInteractiveConfigureCount
              << ", transition=" << m_traceTransitionConfigureCount << ')'
              << " presented=" << m_tracePresentedFrames
              << " resize=" << m_traceResizeApplies
              << " render-nodes=" << m_privateRenderState->nodeCount()
              << " (retained=" << m_privateRenderState->retainedNodeCount()
              << ") render-tx=(create="
              << m_traceRenderCreates
              << ", content=" << m_traceRenderContentUpdates
              << ", props=" << m_traceRenderPropertyUpdates
              << ", remove=" << m_traceRenderRemovals
              << ", scroll-xform="
              << m_traceScrollTransformTransactions
              << ", retained-xform="
              << m_traceRetainedPresentationTransactions
              << ") pipeline-p95=(client="
              << percentile95(m_traceClientBuildSamplesMs)
              << ", raster-queue="
              << percentile95(m_traceRasterQueueSamplesMs)
              << ", raster=" << percentile95(m_traceRasterSamplesMs)
              << ", compose-queue="
              << percentile95(m_traceComposeQueueSamplesMs)
              << ", compose-submit="
              << percentile95(m_traceComposeSubmitSamplesMs)
              << ", end-to-submit="
              << percentile95(m_traceEndToEndSubmitSamplesMs)
              << " ms)\n";
    m_traceLayoutPasses = 0;
    m_traceRenderedFrames = 0;
    m_traceConfigureCount = 0;
    m_traceInteractiveConfigureCount = 0;
    m_traceTransitionConfigureCount = 0;
    m_tracePresentedFrames = 0;
    m_traceResizeApplies = 0;
    m_traceDamagePixels = 0;
    m_traceClearedBytes = 0;
    m_traceRenderCreates = 0;
    m_traceRenderContentUpdates = 0;
    m_traceRenderPropertyUpdates = 0;
    m_traceRenderRemovals = 0;
    m_traceScrollTransformTransactions = 0;
    m_traceRetainedPresentationTransactions = 0;
    m_traceLayoutMs = 0.0;
    m_tracePaintMs = 0.0;
    m_traceRenderTreeMs = 0.0;
    m_traceClearMs = 0.0;
    m_traceDrawMs = 0.0;
    m_traceRasterSubmitMs = 0.0;
    m_traceClientBuildSamplesMs.clear();
    m_traceRasterQueueSamplesMs.clear();
    m_traceRasterSamplesMs.clear();
    m_traceComposeQueueSamplesMs.clear();
    m_traceComposeSubmitSamplesMs.clear();
    m_traceEndToEndSubmitSamplesMs.clear();
    m_traceLastLog = now;
}

} // namespace lcl::ui
