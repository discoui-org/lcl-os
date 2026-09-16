#include "lcl-gpu/vulkan_presentation.hpp"

#include "lcl-gpu/gpu_client.hpp"
#include "platforms/common/native_buffer.hpp"
#include "system/ipc/gpu_protocol.hpp"

#ifndef LCL_HAS_GFXSTREAM_VULKAN_PRESENTATION
#define LCL_HAS_GFXSTREAM_VULKAN_PRESENTATION 0
#endif

#if LCL_HAS_GFXSTREAM_VULKAN_PRESENTATION
#include <vulkan/vk_android_native_buffer.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <string_view>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace lcl::gpu {
namespace {

#if LCL_HAS_GFXSTREAM_VULKAN_PRESENTATION
constexpr char kGfxstreamIcd[] =
    "/System/Library/Gfxstream/icd.d/lcl_gfxstream.json";
#else
constexpr char kNativeIcdDirectory[] =
    "/System/Library/Vulkan/native/icd.d";

bool hasDeviceExtension(VkPhysicalDevice device, std::string_view name) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) !=
        VK_SUCCESS) {
        return false;
    }
    std::vector<VkExtensionProperties> properties(count);
    if (count != 0 &&
        vkEnumerateDeviceExtensionProperties(
            device, nullptr, &count, properties.data()) != VK_SUCCESS) {
        return false;
    }
    return std::any_of(properties.begin(), properties.end(),
                       [name](const VkExtensionProperties& property) {
                           return name == property.extensionName;
                       });
}

bool hasRequiredExtensions(
        VkPhysicalDevice device,
        std::span<const char* const> required) {
    return std::all_of(required.begin(), required.end(),
                       [device](const char* name) {
                           return hasDeviceExtension(device, name);
                       });
}

uint32_t findMemoryType(VkPhysicalDevice device, uint32_t allowed,
                        VkMemoryPropertyFlags desired) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(device, &properties);
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if ((allowed & (1u << index)) != 0 &&
            (properties.memoryTypes[index].propertyFlags & desired) == desired) {
            return index;
        }
    }
    return UINT32_MAX;
}
#endif

#if LCL_HAS_GFXSTREAM_VULKAN_PRESENTATION
class GfxstreamPresentationBackend final : public VulkanPresentation {
public:
    bool prepare(std::string& error) override {
        if (setenv("VK_ICD_FILENAMES", kGfxstreamIcd, 1) != 0) {
            error = std::string{"could not select gfxstream ICD: "} +
                    std::strerror(errno);
            return false;
        }
        if (!broker_.connect() ||
            !broker_.beginHandshake(0x564b474541525331ULL)) {
            error = "could not connect to the GPU broker";
            return false;
        }
        lcl::gpu::DeviceCapabilities capabilities{};
        for (;;) {
            const auto status = broker_.dispatch(capabilities);
            if (status == lcl::gpu::HandshakeStatus::Ready) {
                if (!capabilities.supportsAndroidHardwareBuffer) {
                    error = "GPU broker does not support Android hardware buffers";
                    return false;
                }
                return true;
            }
            if (status != lcl::gpu::HandshakeStatus::Pending) {
                error = "GPU broker handshake failed";
                return false;
            }
            pollfd readable{broker_.fd(), POLLIN, 0};
            int result = -1;
            do {
                result = poll(&readable, 1, 3'000);
            } while (result < 0 && errno == EINTR);
            if (result != 1 || (readable.revents & POLLIN) == 0) {
                error = "GPU broker handshake timed out";
                return false;
            }
        }
    }

    bool selectPhysicalDevice(std::span<const VkPhysicalDevice> devices,
                              VkPhysicalDevice& selected,
                              std::string& error) override {
        if (devices.empty()) {
            error = "gfxstream exposed no Vulkan physical device";
            return false;
        }
        selected = devices.front();
        return true;
    }

    std::span<const char* const> requiredDeviceExtensions() const override {
        return {};
    }

    VkFormat colorFormat() const noexcept override {
        return VK_FORMAT_R8G8B8A8_UNORM;
    }

    bool createTarget(VkPhysicalDevice, VkDevice device,
                      uint32_t width, uint32_t height,
                      VulkanPresentTarget& target,
                      std::string& error) override {
        lcl::gpu::GfxstreamColorBuffer buffer{};
        if (!broker_.createGfxstreamColorBuffer(
                width, height,
                lcl::gpu_protocol::kAndroidHardwareBufferRgba8888,
                buffer)) {
            error = "could not allocate gfxstream color buffer";
            return false;
        }

        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = colorFormat();
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult status = vkCreateImage(device, &imageInfo, nullptr, &target.image);
        if (status != VK_SUCCESS) {
            broker_.destroyGfxstreamColorBuffer(buffer.targetId);
            error = "vkCreateImage failed for gfxstream target";
            return false;
        }
        const VkNativeBufferANDROID nativeBuffer{
            .sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID,
            .handle = &buffer.colorBufferHandle,
            .stride = static_cast<int>(buffer.stride),
            .format = 1,
        };
        const VkBindImageMemoryInfo bindInfo{
            .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
            .pNext = &nativeBuffer,
            .image = target.image,
            .memory = VK_NULL_HANDLE,
        };
        status = vkBindImageMemory2(device, 1, &bindInfo);
        if (status != VK_SUCCESS) {
            vkDestroyImage(device, target.image, nullptr);
            broker_.destroyGfxstreamColorBuffer(buffer.targetId);
            target = {};
            error = "vkBindImageMemory2 failed for gfxstream target";
            return false;
        }
        target.bufferId = buffer.targetId;
        target.width = buffer.width;
        target.height = buffer.height;
        target.stride = buffer.stride * sizeof(uint32_t);
        target.nativeFormat = buffer.format;
        return true;
    }

    bool submitFrame(lcl::client::SurfaceClient& surface,
                     const VulkanPresentTarget& target,
                     uint64_t contentRevision,
                     const lcl::client::Rect& damage,
                     bool opaque,
                     std::string& error) override {
        lcl::client::PlatformNativeFrame frame{};
        frame.bufferId = target.bufferId;
        frame.contentRevision = contentRevision;
        frame.width = target.width;
        frame.height = target.height;
        frame.stride = target.stride;
        frame.format = target.nativeFormat;
        frame.damage = damage;
        frame.opaque = opaque;
        frame.writeHandle = [this, id = target.bufferId](int sidebandFd) {
            return broker_.deliverNativeBuffer(id, sidebandFd);
        };
        if (!surface.submitFrame(std::move(frame))) {
            error = "could not submit gfxstream target to SurfaceClient";
            return false;
        }
        return true;
    }

    bool releasePresentedBuffer(uint64_t bufferId,
                                std::string& error) override {
        if (!broker_.releasePresentedBuffer(bufferId, {})) {
            error = "GPU broker rejected the released gfxstream target";
            return false;
        }
        return true;
    }

    void destroyTarget(VkDevice device,
                       VulkanPresentTarget& target) noexcept override {
        if (target.image != VK_NULL_HANDLE) {
            vkDestroyImage(device, target.image, nullptr);
        }
        if (target.bufferId != 0) {
            broker_.destroyGfxstreamColorBuffer(target.bufferId);
        }
        target = {};
    }

private:
    lcl::gpu::GpuClient broker_;
};
#endif

#if !LCL_HAS_GFXSTREAM_VULKAN_PRESENTATION
class NativeDmaBufPresentationBackend final : public VulkanPresentation {
public:
    ~NativeDmaBufPresentationBackend() override {
        if (renderNode_ >= 0) close(renderNode_);
    }

    bool prepare(std::string& error) override {
        namespace fs = std::filesystem;
        std::vector<std::string> manifests;
        std::error_code filesystemError;
        for (const auto& entry : fs::directory_iterator(
                 kNativeIcdDirectory, filesystemError)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                manifests.push_back(entry.path().string());
            }
        }
        if (filesystemError || manifests.empty()) {
            error = "no native Vulkan ICD is installed";
            return false;
        }
        std::sort(manifests.begin(), manifests.end());
        std::string driverFiles;
        for (const auto& manifest : manifests) {
            if (!driverFiles.empty()) driverFiles.push_back(':');
            driverFiles += manifest;
        }
        if (setenv("VK_DRIVER_FILES", driverFiles.c_str(), 1) != 0 ||
            setenv("VK_ICD_FILENAMES", driverFiles.c_str(), 1) != 0) {
            error = std::string{"could not select native Vulkan ICDs: "} +
                    std::strerror(errno);
            return false;
        }

        for (int index = 128; index <= 143; ++index) {
            const std::string path =
                "/dev/dri/renderD" + std::to_string(index);
            renderNode_ = open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
            if (renderNode_ >= 0) break;
        }
        if (renderNode_ < 0) {
            error = "no sandboxed DRM render node is available";
            return false;
        }
        struct stat status {};
        if (fstat(renderNode_, &status) != 0 || !S_ISCHR(status.st_mode)) {
            error = "sandboxed DRM render node is invalid";
            return false;
        }
        renderMajor_ = major(status.st_rdev);
        renderMinor_ = minor(status.st_rdev);
        return true;
    }

    bool selectPhysicalDevice(std::span<const VkPhysicalDevice> devices,
                              VkPhysicalDevice& selected,
                              std::string& error) override {
        for (const VkPhysicalDevice device : devices) {
            if (!hasRequiredExtensions(device, requiredDeviceExtensions())) {
                continue;
            }
            VkPhysicalDeviceDrmPropertiesEXT drm{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
            VkPhysicalDeviceProperties2 properties{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            properties.pNext = &drm;
            vkGetPhysicalDeviceProperties2(device, &properties);
            if (drm.hasRender == VK_TRUE &&
                drm.renderMajor == renderMajor_ &&
                drm.renderMinor == renderMinor_) {
                selected = device;
                return true;
            }
        }
        error = "no native Vulkan device matches the sandboxed DRM render node";
        return false;
    }

    std::span<const char* const> requiredDeviceExtensions() const override {
        static constexpr std::array<const char*, 3> extensions{
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
            VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
            VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME,
        };
        return extensions;
    }

    VkFormat colorFormat() const noexcept override {
        // DRM_FORMAT_ARGB8888 is byte-compatible with Vulkan BGRA8 on
        // little-endian Linux systems.
        return VK_FORMAT_B8G8R8A8_UNORM;
    }

    bool createTarget(VkPhysicalDevice physicalDevice, VkDevice device,
                      uint32_t width, uint32_t height,
                      VulkanPresentTarget& target,
                      std::string& error) override {
        activeDevice_ = device;
        VkPhysicalDeviceExternalImageFormatInfo externalQuery{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        externalQuery.handleType =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkPhysicalDeviceImageFormatInfo2 formatQuery{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        formatQuery.pNext = &externalQuery;
        formatQuery.format = colorFormat();
        formatQuery.type = VK_IMAGE_TYPE_2D;
        formatQuery.tiling = VK_IMAGE_TILING_LINEAR;
        formatQuery.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        formatQuery.flags = 0;
        VkExternalImageFormatProperties externalProperties{
            VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 formatProperties{
            VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        formatProperties.pNext = &externalProperties;
        const VkResult supported = vkGetPhysicalDeviceImageFormatProperties2(
            physicalDevice, &formatQuery, &formatProperties);
        if (supported != VK_SUCCESS ||
            (externalProperties.externalMemoryProperties.externalMemoryFeatures &
             VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) == 0) {
            error = "native Vulkan device cannot export linear DMA-BUF color targets";
            return false;
        }

        VkExternalMemoryImageCreateInfo externalImage{
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        externalImage.handleTypes =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.pNext = &externalImage;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = colorFormat();
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_LINEAR;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult status = vkCreateImage(
            device, &imageInfo, nullptr, &target.image);
        if (status != VK_SUCCESS) {
            error = "vkCreateImage failed for native DMA-BUF target";
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, target.image, &requirements);
        uint32_t memoryType = findMemoryType(
            physicalDevice, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memoryType == UINT32_MAX) {
            memoryType = findMemoryType(
                physicalDevice, requirements.memoryTypeBits, 0);
        }
        if (memoryType == UINT32_MAX) {
            vkDestroyImage(device, target.image, nullptr);
            target = {};
            error = "no memory type can back the native DMA-BUF target";
            return false;
        }

        VkExportMemoryAllocateInfo exportInfo{
            VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        exportInfo.handleTypes =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkMemoryDedicatedAllocateInfo dedicated{
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.pNext = &exportInfo;
        dedicated.image = target.image;
        VkMemoryAllocateInfo allocation{
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.pNext = &dedicated;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType;
        status = vkAllocateMemory(
            device, &allocation, nullptr, &target.memory);
        if (status == VK_SUCCESS) {
            status = vkBindImageMemory(device, target.image, target.memory, 0);
        }
        if (status != VK_SUCCESS) {
            if (target.memory != VK_NULL_HANDLE) {
                vkFreeMemory(device, target.memory, nullptr);
            }
            vkDestroyImage(device, target.image, nullptr);
            target = {};
            error = "could not allocate native DMA-BUF image memory";
            return false;
        }

        VkImageSubresource subresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout layout{};
        vkGetImageSubresourceLayout(
            device, target.image, &subresource, &layout);
        if (layout.offset != 0 || layout.rowPitch < width * sizeof(uint32_t) ||
            layout.rowPitch > UINT32_MAX) {
            destroyTarget(device, target);
            error = "native DMA-BUF target has an unsupported plane layout";
            return false;
        }
        target.bufferId = nextBufferId_++;
        if (target.bufferId == 0) target.bufferId = nextBufferId_++;
        target.width = width;
        target.height = height;
        target.stride = static_cast<uint32_t>(layout.rowPitch);
        target.nativeFormat = lcl::platform::kDmaBufFormatArgb8888;
        target.modifier = 0; // DRM_FORMAT_MOD_LINEAR
        return true;
    }

    bool submitFrame(lcl::client::SurfaceClient& surface,
                     const VulkanPresentTarget& target,
                     uint64_t contentRevision,
                     const lcl::client::Rect& damage,
                     bool opaque,
                     std::string& error) override {
        const auto getMemoryFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
            vkGetDeviceProcAddr(activeDevice_, "vkGetMemoryFdKHR"));
        if (!getMemoryFd) {
            error = "native Vulkan driver does not expose vkGetMemoryFdKHR";
            return false;
        }
        VkMemoryGetFdInfoKHR fdInfo{
            VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
        fdInfo.memory = target.memory;
        fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        int descriptor = -1;
        const VkResult status = getMemoryFd(activeDevice_, &fdInfo, &descriptor);
        if (status != VK_SUCCESS || descriptor < 0) {
            error = "could not export native Vulkan target as DMA-BUF";
            return false;
        }
        lcl::client::DmaBufFrame frame{};
        frame.bufferId = target.bufferId;
        frame.contentRevision = contentRevision;
        frame.width = target.width;
        frame.height = target.height;
        frame.stride = target.stride;
        frame.format = target.nativeFormat;
        frame.modifier = target.modifier;
        frame.damage = damage;
        frame.opaque = opaque;
        frame.buffer = lcl::client::OwnedFd(descriptor);
        if (!surface.submitFrame(std::move(frame))) {
            error = "could not submit native DMA-BUF target to SurfaceClient";
            return false;
        }
        return true;
    }

    bool releasePresentedBuffer(uint64_t, std::string&) override {
        return true;
    }

    void destroyTarget(VkDevice device,
                       VulkanPresentTarget& target) noexcept override {
        if (target.image != VK_NULL_HANDLE) {
            vkDestroyImage(device, target.image, nullptr);
        }
        if (target.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, target.memory, nullptr);
        }
        target = {};
    }

private:
    int renderNode_{-1};
    unsigned int renderMajor_{0};
    unsigned int renderMinor_{0};
    uint64_t nextBufferId_{1};
    VkDevice activeDevice_{VK_NULL_HANDLE};
};
#endif

} // namespace

std::unique_ptr<VulkanPresentation> createVulkanPresentation() {
#if LCL_HAS_GFXSTREAM_VULKAN_PRESENTATION
    return std::make_unique<GfxstreamPresentationBackend>();
#else
    return std::make_unique<NativeDmaBufPresentationBackend>();
#endif
}

} // namespace lcl::gpu
