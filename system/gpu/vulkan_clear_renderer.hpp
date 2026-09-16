#pragma once

#include "system/ipc/gpu_protocol.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace lcl::gpu {

/** Native Android Vulkan endpoint for the single-command presentation PoC. */
class VulkanClearRenderer final {
public:
    VulkanClearRenderer();
    ~VulkanClearRenderer();
    VulkanClearRenderer(const VulkanClearRenderer&) = delete;
    VulkanClearRenderer& operator=(const VulkanClearRenderer&) = delete;

    bool initialize();
    std::optional<gpu_protocol::ClearColorReady> clear(
        const gpu_protocol::ClearColor& request, int& acquireFenceFd);
    bool deliver(uint64_t bufferId, int sidebandFd);
    void release(uint64_t bufferId, int releaseFenceFd) noexcept;
    void releaseAll() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace lcl::gpu
