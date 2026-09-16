#include <vulkan/vulkan.h>
#include <vulkan/vk_android_native_buffer.h>

#include "lcl-client/surface_client.hpp"
#include "lcl-gpu/gpu_client.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <thread>
#include <variant>

namespace {

constexpr char kIcdPath[] = "/System/Library/Gfxstream/icd.d/lcl_gfxstream.json";
constexpr uint32_t kWidth = 320;
constexpr uint32_t kHeight = 240;

class RunLog final {
public:
    RunLog() : descriptor_(open("/Data/gfxstream-vulkan-test.log",
                                O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)) {}
    ~RunLog() {
        if (descriptor_ >= 0) close(descriptor_);
    }

    void stage(std::string_view value) const {
        if (descriptor_ >= 0) dprintf(descriptor_, "gfxstream_vulkan_test stage=%.*s\n",
                                      static_cast<int>(value.size()), value.data());
    }
    void result(std::string_view value, VkResult status) const {
        if (descriptor_ >= 0) dprintf(descriptor_, "gfxstream_vulkan_test stage=%.*s result=%d\n",
                                      static_cast<int>(value.size()), value.data(), status);
    }

private:
    int descriptor_{-1};
};

int fail(const RunLog& log, std::string_view stage, VkResult status, int code) {
    log.result(stage, status);
    return code;
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

} // namespace

bool completeHandshake(lcl::gpu::GpuClient& client, const RunLog& log) {
    if (!client.connect() || !client.beginHandshake(0x4758465354524541ULL)) {
        log.stage("gpu_broker_connect_failed");
        return false;
    }
    lcl::gpu::DeviceCapabilities capabilities{};
    for (;;) {
        const auto status = client.dispatch(capabilities);
        if (status == lcl::gpu::HandshakeStatus::Ready) return true;
        if (status != lcl::gpu::HandshakeStatus::Pending) {
            log.stage("gpu_broker_handshake_failed");
            return false;
        }
        pollfd readable{client.fd(), POLLIN, 0};
        int result = -1;
        do result = poll(&readable, 1, 3'000); while (result < 0 && errno == EINTR);
        if (result != 1 || (readable.revents & POLLIN) == 0) {
            log.stage("gpu_broker_handshake_timeout");
            return false;
        }
    }
}

int main() {
    RunLog log;
    if (setenv("VK_ICD_FILENAMES", kIcdPath, 1) != 0) {
        log.stage("set_icd_path_failed");
        return 10;
    }
    log.stage("loader_configured");

    lcl::gpu::GpuClient broker;
    if (!completeHandshake(broker, log)) return 10;
    lcl::gpu::GfxstreamColorBuffer target{};

    lcl::client::SurfaceOptions options{};
    options.surfaceId = 1;
    options.appId = "org.lcl.gfxstream-vulkan-test";
    options.title = "Gfxstream Vulkan";
    options.bounds = {96.0f, 144.0f, static_cast<float>(kWidth), static_cast<float>(kHeight)};
    readEnvironmentUint64("LCL_LAUNCH_TOKEN", options.launchToken);
    readEnvironmentUint64("LCL_APP_INSTANCE_ID", options.appInstanceId);
    unsetenv("LCL_LAUNCH_TOKEN");
    unsetenv("LCL_APP_INSTANCE_ID");
    lcl::client::SurfaceClient surface;
    if (!surface.connect(options)) {
        log.stage("surface_connect_failed");
        return 10;
    }
    const auto configureDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < configureDeadline &&
           (!surface.hasConfigure() || surface.rasterFd() < 0)) {
        (void)surface.dispatch();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!surface.hasConfigure() || surface.rasterFd() < 0) {
        surface.disconnect();
        log.stage("surface_configure_timeout");
        return 10;
    }
    log.stage("surface_configured");

    const auto& configure = surface.configure();
    const double scaledWidth =
        std::ceil(static_cast<double>(configure.bounds.width) * configure.bufferScale);
    const double scaledHeight =
        std::ceil(static_cast<double>(configure.bounds.height) * configure.bufferScale);
    if (!std::isfinite(scaledWidth) || !std::isfinite(scaledHeight) ||
        scaledWidth < 1.0 || scaledHeight < 1.0 ||
        scaledWidth > static_cast<double>(UINT32_MAX) ||
        scaledHeight > static_cast<double>(UINT32_MAX)) {
        surface.disconnect();
        log.stage("surface_configure_extent_invalid");
        return 10;
    }
    const uint32_t frameWidth = static_cast<uint32_t>(scaledWidth);
    const uint32_t frameHeight = static_cast<uint32_t>(scaledHeight);
    if (!broker.createGfxstreamColorBuffer(
            frameWidth, frameHeight, lcl::gpu_protocol::kAndroidHardwareBufferRgba8888, target)) {
        surface.disconnect();
        log.stage("gfxstream_target_create_failed");
        return 10;
    }
    log.stage("gfxstream_target_created");

    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkResult status = VK_SUCCESS;
    auto destroyResources = [&]() {
        if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
        if (commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(device, commandPool, nullptr);
        if (memory != VK_NULL_HANDLE) vkFreeMemory(device, memory, nullptr);
        if (image != VK_NULL_HANDLE) vkDestroyImage(device, image, nullptr);
        if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
        if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
        if (target.targetId != 0) {
            (void)broker.destroyGfxstreamColorBuffer(target.targetId);
            target = {};
        }
    };
    auto abortAfterDevice = [&](std::string_view stage) {
        log.result(stage, status);
        destroyResources();
        return 17;
    };

    const VkApplicationInfo applicationInfo{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "LCL Gfxstream Vulkan Test",
        .applicationVersion = 1,
        .pEngineName = "LCL",
        .engineVersion = 1,
        // vkBindImageMemory2 is a Vulkan 1.1 core operation.  The gfxstream
        // virtual device and the Android host both expose 1.1, so declare it
        // here rather than calling a 1.1 dispatch slot from a 1.0 instance.
        .apiVersion = VK_API_VERSION_1_1,
    };
    const VkInstanceCreateInfo instanceInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &applicationInfo,
    };
    status = vkCreateInstance(&instanceInfo, nullptr, &instance);
    if (status != VK_SUCCESS) return fail(log, "vkCreateInstance", status, 11);
    log.stage("vkCreateInstance");

    uint32_t physicalDeviceCount = 0;
    status = vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, nullptr);
    if (status != VK_SUCCESS || physicalDeviceCount == 0) {
        vkDestroyInstance(instance, nullptr);
        return fail(log, "vkEnumeratePhysicalDevices", status, 12);
    }
    std::array<VkPhysicalDevice, 8> physicalDevices{};
    physicalDeviceCount = std::min<uint32_t>(physicalDeviceCount, physicalDevices.size());
    status = vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, physicalDevices.data());
    if (status != VK_SUCCESS) {
        vkDestroyInstance(instance, nullptr);
        return fail(log, "vkEnumeratePhysicalDevices", status, 13);
    }
    const VkPhysicalDevice physicalDevice = physicalDevices[0];
    log.stage("vkEnumeratePhysicalDevices");

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
    std::array<VkQueueFamilyProperties, 8> queueFamilies{};
    queueFamilyCount = std::min<uint32_t>(queueFamilyCount, queueFamilies.size());
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());
    uint32_t queueFamily = UINT32_MAX;
    for (uint32_t index = 0; index < queueFamilyCount; ++index) {
        if ((queueFamilies[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            queueFamily = index;
            break;
        }
    }
    if (queueFamily == UINT32_MAX) {
        vkDestroyInstance(instance, nullptr);
        log.stage("graphics_queue_missing");
        return 14;
    }

    constexpr float queuePriority = 1.0f;
    const VkDeviceQueueCreateInfo queueInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queueFamily,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority,
    };
    const VkDeviceCreateInfo deviceInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueInfo,
    };
    status = vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device);
    if (status != VK_SUCCESS) {
        vkDestroyInstance(instance, nullptr);
        return fail(log, "vkCreateDevice", status, 15);
    }
    log.stage("vkCreateDevice");

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    if (queue == VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        log.stage("vkGetDeviceQueue_failed");
        return 16;
    }

    const VkImageCreateInfo imageInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {target.width, target.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    status = vkCreateImage(device, &imageInfo, nullptr, &image);
    if (status != VK_SUCCESS) return abortAfterDevice("vkCreateImage");
    log.stage("vkCreateImage");
    const VkNativeBufferANDROID nativeBuffer{
        .sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID,
        .handle = &target.colorBufferHandle,
        .stride = static_cast<int>(target.stride),
        .format = 1, // HAL_PIXEL_FORMAT_RGBA_8888
    };
    const VkBindImageMemoryInfo bindImage{
        .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
        .pNext = &nativeBuffer,
        .image = image,
        .memory = VK_NULL_HANDLE,
    };
    status = vkBindImageMemory2(device, 1, &bindImage);
    if (status != VK_SUCCESS) return abortAfterDevice("vkBindImageMemory2_native_buffer");
    log.stage("vkBindImageMemory2_native_buffer");

    const VkCommandPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = queueFamily,
    };
    status = vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool);
    if (status != VK_SUCCESS) return abortAfterDevice("vkCreateCommandPool");
    const VkCommandBufferAllocateInfo commandBufferInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    status = vkAllocateCommandBuffers(device, &commandBufferInfo, &commandBuffer);
    if (status != VK_SUCCESS) return abortAfterDevice("vkAllocateCommandBuffers");
    const VkCommandBufferBeginInfo beginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    status = vkBeginCommandBuffer(commandBuffer, &beginInfo);
    if (status != VK_SUCCESS) return abortAfterDevice("vkBeginCommandBuffer");
    const VkImageMemoryBarrier toTransfer{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toTransfer);
    const VkClearColorValue color{{0.12f, 0.58f, 0.92f, 1.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                         &range);
    status = vkEndCommandBuffer(commandBuffer);
    if (status != VK_SUCCESS) return abortAfterDevice("vkEndCommandBuffer");
    log.stage("vkCmdClearColorImage");

    const VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    status = vkCreateFence(device, &fenceInfo, nullptr, &fence);
    if (status != VK_SUCCESS) return abortAfterDevice("vkCreateFence");
    const VkSubmitInfo submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &commandBuffer,
    };
    status = vkQueueSubmit(queue, 1, &submitInfo, fence);
    if (status != VK_SUCCESS) return abortAfterDevice("vkQueueSubmit");
    log.stage("vkQueueSubmit");
    status = vkWaitForFences(device, 1, &fence, VK_TRUE, 3'000'000'000ULL);
    if (status != VK_SUCCESS) return abortAfterDevice("vkWaitForFences");
    log.stage("vkWaitForFences");
    status = vkQueueWaitIdle(queue);
    if (status != VK_SUCCESS) return abortAfterDevice("vkQueueWaitIdle");
    lcl::client::PlatformNativeFrame frame{};
    frame.bufferId = target.targetId;
    frame.contentRevision = 1;
    frame.width = target.width;
    frame.height = target.height;
    frame.stride = target.stride * sizeof(uint32_t);
    frame.format = lcl::gpu_protocol::kAndroidHardwareBufferRgba8888;
    frame.damage = {0.0f, 0.0f, configure.bounds.width, configure.bounds.height};
    frame.opaque = true;
    frame.writeHandle = [&broker, targetId = target.targetId](const int sidebandFd) {
        return broker.deliverNativeBuffer(targetId, sidebandFd);
    };
    if (!surface.submitFrame(std::move(frame))) {
        return abortAfterDevice("surface_submit_native_buffer");
    }
    log.stage("surface_submit_native_buffer");

    bool presented = false;
    bool released = false;
    bool closeRequested = false;
    const auto visibleUntil = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    const auto releaseDeadline = visibleUntil + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < releaseDeadline && !released) {
        for (auto& event : surface.dispatch()) {
            if (std::get_if<lcl::client::FramePresentedEvent>(&event)) {
                presented = true;
            } else if (auto* release = std::get_if<lcl::client::BufferReleasedEvent>(&event);
                       release && release->bufferId == target.targetId) {
                (void)broker.releasePresentedBuffer(target.targetId,
                                                    std::move(release->releaseFence));
                released = true;
            }
        }
        if (!closeRequested && std::chrono::steady_clock::now() >= visibleUntil) {
            closeRequested = surface.requestSurfaceClose();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!closeRequested) (void)surface.requestSurfaceClose();
    if (!presented || !released) {
        log.stage("surface_presentation_timeout");
        destroyResources();
        return 18;
    }
    log.stage("complete");

    destroyResources();
    return 0;
}
