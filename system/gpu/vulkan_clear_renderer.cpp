#include "system/gpu/vulkan_clear_renderer.hpp"

#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <android/hardware_buffer.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace lcl::gpu {
namespace {

bool findMemoryType(VkPhysicalDevice device, uint32_t bits, uint32_t& index) {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(device, &memory);
    for (uint32_t candidate = 0; candidate < memory.memoryTypeCount; ++candidate) {
        if ((bits & (1u << candidate)) != 0) {
            index = candidate;
            return true;
        }
    }
    return false;
}

bool validRequest(const gpu_protocol::ClearColor& request) {
    return request.requestId != 0 && request.width > 0 && request.height > 0 &&
           request.width <= 16384 && request.height <= 16384 &&
           request.format == gpu_protocol::kAndroidHardwareBufferRgba8888 &&
           request.usage == gpu_protocol::kAndroidHardwareBufferGpuSampledColorOutput &&
           std::isfinite(request.red) && std::isfinite(request.green) &&
           std::isfinite(request.blue) && std::isfinite(request.alpha) &&
           request.red >= 0.0f && request.red <= 1.0f && request.green >= 0.0f &&
           request.green <= 1.0f && request.blue >= 0.0f && request.blue <= 1.0f &&
           request.alpha >= 0.0f && request.alpha <= 1.0f;
}

} // namespace

struct VulkanClearRenderer::Impl {
    struct Buffer {
        AHardwareBuffer* hardwareBuffer{nullptr};
        VkImage image{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkCommandBuffer command{VK_NULL_HANDLE};
        bool delivered{false};
    };

    VkInstance instance{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue queue{VK_NULL_HANDLE};
    VkCommandPool commandPool{VK_NULL_HANDLE};
    uint32_t queueFamily{0};
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID getAhbProperties{nullptr};
    PFN_vkGetFenceFdKHR getFenceFd{nullptr};
    uint64_t nextBufferId{1};
    std::unordered_map<uint64_t, Buffer> buffers;

    void destroy(Buffer& buffer) noexcept {
        if (device != VK_NULL_HANDLE && buffer.command != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(device, commandPool, 1, &buffer.command);
        }
        if (device != VK_NULL_HANDLE && buffer.image != VK_NULL_HANDLE) {
            vkDestroyImage(device, buffer.image, nullptr);
        }
        if (device != VK_NULL_HANDLE && buffer.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, buffer.memory, nullptr);
        }
        if (buffer.hardwareBuffer) AHardwareBuffer_release(buffer.hardwareBuffer);
        buffer = {};
    }
};

VulkanClearRenderer::VulkanClearRenderer() : m_impl(std::make_unique<Impl>()) {}
VulkanClearRenderer::~VulkanClearRenderer() {
    releaseAll();
    if (m_impl->device != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_impl->device, m_impl->commandPool, nullptr);
        vkDestroyDevice(m_impl->device, nullptr);
    }
    if (m_impl->instance != VK_NULL_HANDLE) vkDestroyInstance(m_impl->instance, nullptr);
}

bool VulkanClearRenderer::initialize() {
    if (m_impl->device != VK_NULL_HANDLE) return true;
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create.pApplicationInfo = &application;
    if (vkCreateInstance(&create, nullptr, &m_impl->instance) != VK_SUCCESS) return false;
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(m_impl->instance, &count, nullptr) != VK_SUCCESS || count == 0) return false;
    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(m_impl->instance, &count, devices.data()) != VK_SUCCESS) return false;
    for (VkPhysicalDevice candidate : devices) {
        uint32_t queues = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queues, nullptr);
        std::vector<VkQueueFamilyProperties> properties(queues);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queues, properties.data());
        for (uint32_t index = 0; index < queues; ++index) {
            if ((properties[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                m_impl->physicalDevice = candidate;
                m_impl->queueFamily = index;
                break;
            }
        }
        if (m_impl->physicalDevice != VK_NULL_HANDLE) break;
    }
    if (m_impl->physicalDevice == VK_NULL_HANDLE) return false;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = m_impl->queueFamily;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    const char* extensions[] = {
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
        VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
    };
    VkDeviceCreateInfo device{};
    device.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device.queueCreateInfoCount = 1;
    device.pQueueCreateInfos = &queue;
    device.enabledExtensionCount = std::size(extensions);
    device.ppEnabledExtensionNames = extensions;
    if (vkCreateDevice(m_impl->physicalDevice, &device, nullptr, &m_impl->device) != VK_SUCCESS) return false;
    vkGetDeviceQueue(m_impl->device, m_impl->queueFamily, 0, &m_impl->queue);
    m_impl->getAhbProperties = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
        vkGetDeviceProcAddr(m_impl->device, "vkGetAndroidHardwareBufferPropertiesANDROID"));
    m_impl->getFenceFd = reinterpret_cast<PFN_vkGetFenceFdKHR>(
        vkGetDeviceProcAddr(m_impl->device, "vkGetFenceFdKHR"));
    if (!m_impl->getAhbProperties || !m_impl->getFenceFd) return false;
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.queueFamilyIndex = m_impl->queueFamily;
    if (vkCreateCommandPool(m_impl->device, &pool, nullptr, &m_impl->commandPool) != VK_SUCCESS) return false;
    return true;
}

std::optional<gpu_protocol::ClearColorReady> VulkanClearRenderer::clear(
        const gpu_protocol::ClearColor& request, int& acquireFenceFd) {
    acquireFenceFd = -1;
    if (!initialize() || !validRequest(request)) return std::nullopt;
    Impl::Buffer buffer{};
    AHardwareBuffer_Desc description{};
    description.width = request.width;
    description.height = request.height;
    description.layers = 1;
    description.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    description.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                        AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
    if (AHardwareBuffer_allocate(&description, &buffer.hardwareBuffer) != 0 || !buffer.hardwareBuffer) return std::nullopt;
    AHardwareBuffer_describe(buffer.hardwareBuffer, &description);

    VkExternalMemoryImageCreateInfo external{};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo image{};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.pNext = &external;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.extent = {request.width, request.height, 1};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_impl->device, &image, nullptr, &buffer.image) != VK_SUCCESS) {
        m_impl->destroy(buffer);
        return std::nullopt;
    }
    VkAndroidHardwareBufferPropertiesANDROID properties{};
    properties.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    if (m_impl->getAhbProperties(m_impl->device, buffer.hardwareBuffer, &properties) != VK_SUCCESS) {
        m_impl->destroy(buffer);
        return std::nullopt;
    }
    uint32_t memoryType = 0;
    if (!findMemoryType(m_impl->physicalDevice, properties.memoryTypeBits, memoryType)) {
        m_impl->destroy(buffer);
        return std::nullopt;
    }
    VkImportAndroidHardwareBufferInfoANDROID imported{};
    imported.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    imported.buffer = buffer.hardwareBuffer;
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.pNext = &imported;
    allocation.allocationSize = properties.allocationSize;
    allocation.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(m_impl->device, &allocation, nullptr, &buffer.memory) != VK_SUCCESS ||
        vkBindImageMemory(m_impl->device, buffer.image, buffer.memory, 0) != VK_SUCCESS) {
        m_impl->destroy(buffer);
        return std::nullopt;
    }
    VkCommandBufferAllocateInfo command{};
    command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command.commandPool = m_impl->commandPool;
    command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(m_impl->device, &command, &buffer.command) != VK_SUCCESS) {
        m_impl->destroy(buffer);
        return std::nullopt;
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(buffer.command, &begin) != VK_SUCCESS) {
        m_impl->destroy(buffer);
        return std::nullopt;
    }
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.image = buffer.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(buffer.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkClearColorValue color{{request.red, request.green, request.blue, request.alpha}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(buffer.command, buffer.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &color, 1, &range);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(buffer.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    if (vkEndCommandBuffer(buffer.command) != VK_SUCCESS) {
        m_impl->destroy(buffer);
        return std::nullopt;
    }
    VkExportFenceCreateInfo exported{};
    exported.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO;
    exported.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
    VkFenceCreateInfo fenceCreate{};
    fenceCreate.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceCreate.pNext = &exported;
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(m_impl->device, &fenceCreate, nullptr, &fence) != VK_SUCCESS) {
        m_impl->destroy(buffer);
        return std::nullopt;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &buffer.command;
    if (vkQueueSubmit(m_impl->queue, 1, &submit, fence) != VK_SUCCESS) {
        vkDestroyFence(m_impl->device, fence, nullptr);
        m_impl->destroy(buffer);
        return std::nullopt;
    }
    VkFenceGetFdInfoKHR fenceFd{};
    fenceFd.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR;
    fenceFd.fence = fence;
    fenceFd.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
    if (m_impl->getFenceFd(m_impl->device, &fenceFd, &acquireFenceFd) != VK_SUCCESS || acquireFenceFd < 0) {
        vkDestroyFence(m_impl->device, fence, nullptr);
        m_impl->destroy(buffer);
        return std::nullopt;
    }
    vkDestroyFence(m_impl->device, fence, nullptr);
    const uint64_t bufferId = m_impl->nextBufferId++;
    m_impl->buffers.emplace(bufferId, std::move(buffer));
    return gpu_protocol::ClearColorReady{request.requestId, bufferId, 1,
                                         request.width, request.height,
                                         static_cast<uint32_t>(description.stride * sizeof(uint32_t)),
                                         request.format};
}

bool VulkanClearRenderer::deliver(uint64_t bufferId, int sidebandFd) {
    const auto found = m_impl->buffers.find(bufferId);
    if (found == m_impl->buffers.end() || found->second.delivered || sidebandFd < 0) return false;
    if (AHardwareBuffer_sendHandleToUnixSocket(found->second.hardwareBuffer, sidebandFd) != 0) return false;
    found->second.delivered = true;
    return true;
}

void VulkanClearRenderer::release(uint64_t bufferId, int releaseFenceFd) noexcept {
    if (releaseFenceFd >= 0) close(releaseFenceFd);
    const auto found = m_impl->buffers.find(bufferId);
    if (found == m_impl->buffers.end()) return;
    m_impl->destroy(found->second);
    m_impl->buffers.erase(found);
}

void VulkanClearRenderer::releaseAll() noexcept {
    for (auto& [_, buffer] : m_impl->buffers) m_impl->destroy(buffer);
    m_impl->buffers.clear();
}

} // namespace lcl::gpu
