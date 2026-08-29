#pragma once

#include "core/compositor/surface_registry.hpp"
#include "platform/common/display_backend.hpp"
#include "render/renderer.hpp"
#include "render/window_manager.hpp"

#include <functional>
#include <unordered_map>
#include <vector>

namespace lcl::core {

/** Draws and presents one immutable surface snapshot through the compositor backend. */
class CompositorRenderer {
public:
    void render(render::Renderer& renderer,
                lcl::platform::IDisplayBackend& displayBackend,
                const render::WindowManager& windowManager,
                const SurfaceRegistry::Snapshot& surfaces,
                const std::function<void()>& beforePresent = {},
                bool allowIncrementalDamage = false,
                bool useMobilePresentation = false) const;

private:
    struct RetainedWindowGroup {
        uint32_t framebuffer{0};
        uint32_t texture{0};
        uint32_t pixelWidth{0};
        uint32_t pixelHeight{0};
        uint64_t resourceGeneration{0};
        graphics::RectF bounds{};
        float cornerRadius{0.0f};
        float cornerRoundness{2.0f};
        std::vector<uint32_t> pixels;
        bool valid{false};
    };

    struct ComposedSurfaceFrame {
        uint64_t frameSerial{0};
        uint64_t shmContentSerial{0};
        uint64_t rasterLayerId{0};
    };

    struct RetainedBackdropBase {
        uint32_t framebuffer{0};
        uint32_t texture{0};
        uint32_t pixelWidth{0};
        uint32_t pixelHeight{0};
        uint64_t resourceGeneration{0};
        uint64_t effectRevision{0};
        graphics::RectF bounds{};
        float windowOpacity{1.0f};
        float windowScale{1.0f};
        bool valid{false};
    };

    // The GPU compositor scene FBO is authoritative across frames. Partial
    // damage is enabled only after one complete frame initialized it.
    mutable bool m_hasCompleteRetainedFrame{false};
    mutable uint64_t m_retainedFrameResourceGeneration{0};
    mutable uint32_t m_retainedFrameWidth{0};
    mutable uint32_t m_retainedFrameHeight{0};
    mutable std::unordered_map<SurfaceRegistry::Key, ComposedSurfaceFrame>
        m_composedSurfaceFrames;
    // Pre-client WindowGroup pixels after backdrop effects. A client-only
    // damage frame can restore this base and composite its new retained layer
    // without executing an unchanged full-surface blur again.
    mutable std::unordered_map<SurfaceRegistry::Key, RetainedBackdropBase>
        m_retainedBackdropBases;
    mutable std::unordered_map<uint32_t, RetainedWindowGroup>
        m_retainedWindowGroups;
};

} // namespace lcl::core
