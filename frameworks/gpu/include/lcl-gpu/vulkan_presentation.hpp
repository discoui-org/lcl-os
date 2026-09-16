#pragma once

#include "lcl-client/surface_client.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace lcl::gpu {

/**
 * One reusable Vulkan render target whose transport is owned by the selected
 * platform provider.  The application renders only through the Vulkan image
 * and identifies the target by bufferId; native handles never escape this ABI.
 */
struct VulkanPresentTarget {
    uint64_t bufferId{0};
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t stride{0};
    uint32_t nativeFormat{0};
    uint64_t modifier{~uint64_t{0}};
};

/**
 * Platform substrate for Vulkan presentation. Applications retain normal
 * Vulkan command submission; this provider owns only device selection,
 * shareable targets, synchronization handoff, and SurfaceClient transport.
 */
class VulkanPresentation {
public:
    virtual ~VulkanPresentation() = default;

    /** Selects the installed provider and prepares its Vulkan loader view. */
    virtual bool prepare(std::string& error) = 0;
    virtual bool selectPhysicalDevice(std::span<const VkPhysicalDevice> devices,
                                      VkPhysicalDevice& selected,
                                      std::string& error) = 0;
    virtual std::span<const char* const> requiredDeviceExtensions() const = 0;
    virtual VkFormat colorFormat() const noexcept = 0;

    virtual bool createTarget(VkPhysicalDevice physicalDevice, VkDevice device,
                              uint32_t width, uint32_t height,
                              VulkanPresentTarget& target,
                              std::string& error) = 0;
    virtual bool submitFrame(lcl::client::SurfaceClient& surface,
                             const VulkanPresentTarget& target,
                             uint64_t contentRevision,
                             const lcl::client::Rect& damage,
                             bool opaque,
                             std::string& error) = 0;
    virtual bool releasePresentedBuffer(uint64_t bufferId,
                                        std::string& error) = 0;
    virtual void destroyTarget(VkDevice device,
                               VulkanPresentTarget& target) noexcept = 0;
};

/** Runtime capability selection; callers never branch on the host platform. */
std::unique_ptr<VulkanPresentation> createVulkanPresentation();

} // namespace lcl::gpu
