#include <vulkan/vulkan.h>
#include <vulkan/vk_android_native_buffer.h>

#include "lcl-client/surface_client.hpp"
#include "lcl-gpu/gpu_client.hpp"
#include "shaders.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;
using Seconds = std::chrono::duration<double>;

constexpr char kIcdPath[] = "/System/Library/Gfxstream/icd.d/lcl_gfxstream.json";
constexpr uint32_t kBufferCount = 6;
constexpr uint32_t kRenderAheadCount = 2;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegreesToRadians = kPi / 180.0f;
constexpr float kGearScale = 0.42f;

class RunLog final {
public:
    RunLog() : descriptor_(open("/Data/vulkan-gears.log",
                                O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)) {}
    ~RunLog() { if (descriptor_ >= 0) close(descriptor_); }

    void stage(std::string_view value) const {
        if (descriptor_ >= 0) {
            dprintf(descriptor_, "vulkan_gears stage=%.*s\n",
                    static_cast<int>(value.size()), value.data());
        }
    }

    void vulkanError(std::string_view operation, VkResult status) const {
        if (descriptor_ >= 0) {
            dprintf(descriptor_, "vulkan_gears error=%.*s vk_result=%d\n",
                    static_cast<int>(operation.size()), operation.data(), status);
        }
    }

    void syncError(uint64_t bufferId) const {
        if (descriptor_ >= 0) {
            dprintf(descriptor_, "vulkan_gears error=release_fence_timeout buffer=%llu\n",
                    static_cast<unsigned long long>(bufferId));
        }
    }

    void metrics(uint64_t submitted, uint64_t presented, double fps,
                 double frameMs, double submitUs, double presentMs,
                 uint64_t dropped, uint64_t rejected) const {
        if (descriptor_ >= 0) {
            dprintf(descriptor_,
                    "vulkan_gears frames_submitted=%llu frames_presented=%llu "
                    "fps=%.2f avg_frame_ms=%.3f avg_submit_us=%.2f "
                    "avg_present_ms=%.3f dropped=%llu rejected=%llu\n",
                    static_cast<unsigned long long>(submitted),
                    static_cast<unsigned long long>(presented), fps, frameMs,
                    submitUs, presentMs,
                    static_cast<unsigned long long>(dropped),
                    static_cast<unsigned long long>(rejected));
        }
    }

private:
    int descriptor_{-1};
};

bool readEnvironmentUint64(const char* name, uint64_t& value) {
    const char* text = std::getenv(name);
    if (!text || text[0] == '\0') return false;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' || parsed == 0) return false;
    value = static_cast<uint64_t>(parsed);
    return true;
}

bool completeHandshake(lcl::gpu::GpuClient& client, const RunLog& log) {
    if (!client.connect() || !client.beginHandshake(0x564b474541525331ULL)) {
        log.stage("gpu_broker_connect_failed");
        return false;
    }
    lcl::gpu::DeviceCapabilities capabilities{};
    for (;;) {
        const auto status = client.dispatch(capabilities);
        if (status == lcl::gpu::HandshakeStatus::Ready) {
            return capabilities.supportsAndroidHardwareBuffer;
        }
        if (status != lcl::gpu::HandshakeStatus::Pending) return false;
        pollfd readable{client.fd(), POLLIN, 0};
        int result = -1;
        do result = poll(&readable, 1, 3'000); while (result < 0 && errno == EINTR);
        if (result != 1 || (readable.revents & POLLIN) == 0) return false;
    }
}

struct Mat4 {
    std::array<float, 16> values{};
};

Mat4 identity() {
    Mat4 value{};
    value.values[0] = value.values[5] = value.values[10] = value.values[15] = 1.0f;
    return value;
}

Mat4 multiply(const Mat4& left, const Mat4& right) {
    Mat4 result{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            for (int inner = 0; inner < 4; ++inner) {
                result.values[column * 4 + row] +=
                    left.values[inner * 4 + row] * right.values[column * 4 + inner];
            }
        }
    }
    return result;
}

Mat4 translation(float x, float y, float z) {
    Mat4 value = identity();
    value.values[12] = x;
    value.values[13] = y;
    value.values[14] = z;
    return value;
}

Mat4 rotationX(float angle) {
    Mat4 value = identity();
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    value.values[5] = cosine;
    value.values[6] = sine;
    value.values[9] = -sine;
    value.values[10] = cosine;
    return value;
}

Mat4 rotationY(float angle) {
    Mat4 value = identity();
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    value.values[0] = cosine;
    value.values[2] = -sine;
    value.values[8] = sine;
    value.values[10] = cosine;
    return value;
}

Mat4 rotationZ(float angle) {
    Mat4 value = identity();
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    value.values[0] = cosine;
    value.values[1] = sine;
    value.values[4] = -sine;
    value.values[5] = cosine;
    return value;
}

Mat4 perspective(float verticalFov, float aspect, float nearPlane, float farPlane) {
    Mat4 value{};
    const float focal = 1.0f / std::tan(verticalFov * 0.5f);
    value.values[0] = focal / aspect;
    value.values[5] = -focal;
    value.values[10] = farPlane / (nearPlane - farPlane);
    value.values[11] = -1.0f;
    value.values[14] = (nearPlane * farPlane) / (nearPlane - farPlane);
    return value;
}

struct Vertex {
    float position[3];
    float normal[3];
    float color[3];
};

struct GearRange {
    uint32_t firstIndex{0};
    uint32_t indexCount{0};
    float x{0.0f};
    float y{0.0f};
    float speed{1.0f};
    float phase{0.0f};
};

void appendQuad(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
                const std::array<std::array<float, 3>, 4>& positions,
                const std::array<float, 3>& normal,
                const std::array<float, 3>& color, bool reverse = false) {
    const uint32_t base = static_cast<uint32_t>(vertices.size());
    for (const auto& position : positions) {
        vertices.push_back({{position[0], position[1], position[2]},
                            {normal[0], normal[1], normal[2]},
                            {color[0], color[1], color[2]}});
    }
    if (!reverse) {
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    } else {
        indices.insert(indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
    }
}

GearRange appendGear(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
                     uint32_t teeth, float innerRadius, float rootRadius,
                     float outerRadius, float width, std::array<float, 3> color,
                     float x, float y, float speed, float phase) {
    GearRange range{static_cast<uint32_t>(indices.size()), 0, x, y, speed, phase};
    const uint32_t segments = teeth * 4;
    const float halfWidth = width * 0.5f;
    auto radiusAt = [rootRadius, outerRadius](uint32_t segment) {
        const uint32_t toothPart = segment % 4;
        return toothPart == 1 || toothPart == 2 ? outerRadius : rootRadius;
    };
    auto point = [](float radius, float angle, float z) {
        return std::array<float, 3>{radius * std::cos(angle), radius * std::sin(angle), z};
    };
    for (uint32_t segment = 0; segment < segments; ++segment) {
        const uint32_t next = (segment + 1) % segments;
        const float angle0 = 2.0f * kPi * static_cast<float>(segment) /
                             static_cast<float>(segments);
        const float angle1 = 2.0f * kPi * static_cast<float>(next) /
                             static_cast<float>(segments);
        const float radius0 = radiusAt(segment);
        const float radius1 = radiusAt(next);
        const auto inner0Front = point(innerRadius, angle0, halfWidth);
        const auto inner1Front = point(innerRadius, angle1, halfWidth);
        const auto outer0Front = point(radius0, angle0, halfWidth);
        const auto outer1Front = point(radius1, angle1, halfWidth);
        const auto inner0Back = point(innerRadius, angle0, -halfWidth);
        const auto inner1Back = point(innerRadius, angle1, -halfWidth);
        const auto outer0Back = point(radius0, angle0, -halfWidth);
        const auto outer1Back = point(radius1, angle1, -halfWidth);
        appendQuad(vertices, indices,
                   {inner0Front, outer0Front, outer1Front, inner1Front},
                   {0.0f, 0.0f, 1.0f}, color);
        appendQuad(vertices, indices,
                   {inner0Back, inner1Back, outer1Back, outer0Back},
                   {0.0f, 0.0f, -1.0f}, color);
        const float middle = (angle0 + angle1) * 0.5f;
        appendQuad(vertices, indices,
                   {outer0Back, outer1Back, outer1Front, outer0Front},
                   {std::cos(middle), std::sin(middle), 0.0f}, color);
        appendQuad(vertices, indices,
                   {inner0Back, inner0Front, inner1Front, inner1Back},
                   {-std::cos(middle), -std::sin(middle), 0.0f}, color);
    }
    range.indexCount = static_cast<uint32_t>(indices.size()) - range.firstIndex;
    return range;
}

uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t allowed,
                        VkMemoryPropertyFlags desired) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if ((allowed & (1u << index)) != 0 &&
            (properties.memoryTypes[index].propertyFlags & desired) == desired) {
            return index;
        }
    }
    return UINT32_MAX;
}

enum class FencePoll { Pending, Ready, Error };

FencePoll pollReleaseFence(lcl::client::OwnedFd& fence) {
    if (!fence) return FencePoll::Ready;
    pollfd ready{fence.get(), POLLIN, 0};
    int result = -1;
    do result = poll(&ready, 1, 0); while (result < 0 && errno == EINTR);
    if (result == 0) return FencePoll::Pending;
    if (result == 1 && (ready.revents & (POLLIN | POLLHUP)) != 0) {
        return FencePoll::Ready;
    }
    return FencePoll::Error;
}

struct PresentSlot {
    lcl::gpu::GfxstreamColorBuffer target{};
    VkImage image{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkFramebuffer framebuffer{VK_NULL_HANDLE};
    VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
    VkFence renderFence{VK_NULL_HANDLE};
    VkSemaphore renderComplete{VK_NULL_HANDLE};
    uint64_t revision{1};
    uint64_t gpuSequence{0};
    bool busy{false};
    bool gpuPending{false};
    bool readyToPresent{false};
    bool releasePending{false};
    lcl::client::OwnedFd releaseFence{};
};

struct PushConstants {
    Mat4 mvp;
    Mat4 model;
};
static_assert(sizeof(PushConstants) == 128);

class VulkanRenderer final {
public:
    VulkanRenderer(lcl::gpu::GpuClient& broker, const RunLog& log)
        : broker_(broker), log_(log) {}
    ~VulkanRenderer() { destroy(); }

    bool initialize(uint32_t width, uint32_t height) {
        width_ = width;
        height_ = height;
        if (!createDevice()) return stageFailure("device_init_failed");
        log_.stage("device_ready");
        if (!createGeometry()) return stageFailure("geometry_init_failed");
        log_.stage("geometry_ready");
        if (!createRenderPass()) return stageFailure("render_pass_init_failed");
        log_.stage("render_pass_ready");
        if (!createDepthBuffer()) return stageFailure("depth_init_failed");
        log_.stage("depth_ready");
        if (!createTargets()) return stageFailure("targets_init_failed");
        log_.stage("targets_ready");
        if (!createPipeline()) return stageFailure("pipeline_init_failed");
        log_.stage("pipeline_ready");
        if (!createCommands()) return stageFailure("commands_init_failed");
        log_.stage("graphics_pipeline_ready");
        return true;
    }

    PresentSlot* availableSlot() {
        const auto found = std::find_if(slots_.begin(), slots_.end(),
            [](const PresentSlot& slot) {
                return !slot.busy && !slot.gpuPending && !slot.readyToPresent &&
                       !slot.releasePending;
            });
        return found == slots_.end() ? nullptr : &*found;
    }

    PresentSlot* readySlot() {
        PresentSlot* selected = nullptr;
        for (auto& slot : slots_) {
            if (!slot.readyToPresent) continue;
            if (!selected || slot.gpuSequence < selected->gpuSequence) selected = &slot;
        }
        return selected;
    }

    uint32_t renderAheadCount() const {
        return static_cast<uint32_t>(std::count_if(
            slots_.begin(), slots_.end(), [](const PresentSlot& slot) {
                return slot.gpuPending || slot.readyToPresent;
            }));
    }

    PresentSlot* slotFor(uint64_t bufferId) {
        const auto found = std::find_if(slots_.begin(), slots_.end(),
            [bufferId](const PresentSlot& slot) { return slot.target.targetId == bufferId; });
        return found == slots_.end() ? nullptr : &*found;
    }

    bool collectReleasedBuffers() {
        for (auto& slot : slots_) {
            if (!slot.releasePending) continue;
            switch (pollReleaseFence(slot.releaseFence)) {
                case FencePoll::Pending:
                    continue;
                case FencePoll::Error:
                    log_.syncError(slot.target.targetId);
                    return false;
                case FencePoll::Ready:
                    break;
            }
            if (!broker_.releasePresentedBuffer(slot.target.targetId, {})) {
                log_.stage("broker_release_failed");
                return false;
            }
            slot.releaseFence.reset();
            slot.releasePending = false;
            slot.busy = false;
            ++slot.revision;
            if (slot.revision == 0) slot.revision = 1;
        }
        return true;
    }

    bool collectGpuCompletions() {
        PresentSlot* oldest = nullptr;
        for (auto& slot : slots_) {
            if (!slot.gpuPending) continue;
            if (!oldest || slot.gpuSequence < oldest->gpuSequence) oldest = &slot;
        }
        if (!oldest) return true;
        const VkResult result = vkGetFenceStatus(device_, oldest->renderFence);
        if (result == VK_NOT_READY) return true;
        if (result != VK_SUCCESS) return fail("vkGetFenceStatus", result);
        oldest->gpuPending = false;
        oldest->readyToPresent = true;
        return true;
    }

    bool render(PresentSlot& slot, float timeSeconds, double& submitMicroseconds) {
        if (vkResetFences(device_, 1, &slot.renderFence) != VK_SUCCESS ||
            vkResetCommandBuffer(slot.commandBuffer, 0) != VK_SUCCESS) {
            return false;
        }
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(slot.commandBuffer, &begin) != VK_SUCCESS) return false;

        if (!geometryUploaded_) {
            vkCmdUpdateBuffer(slot.commandBuffer, vertexBuffer_, 0,
                              vertexUpload_.size() * sizeof(Vertex), vertexUpload_.data());
            vkCmdUpdateBuffer(slot.commandBuffer, indexBuffer_, 0,
                              indexUpload_.size() * sizeof(uint32_t), indexUpload_.data());
            const std::array<VkBufferMemoryBarrier, 2> barriers{{
                {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
                 VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                 vertexBuffer_, 0, VK_WHOLE_SIZE},
                {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_INDEX_READ_BIT,
                 VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                 indexBuffer_, 0, VK_WHOLE_SIZE},
            }};
            vkCmdPipelineBarrier(slot.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr,
                                 static_cast<uint32_t>(barriers.size()), barriers.data(),
                                 0, nullptr);
        }

        const std::array<VkClearValue, 2> clears{{
            {.color = {{0.018f, 0.024f, 0.045f, 1.0f}}},
            {.depthStencil = {1.0f, 0}},
        }};
        VkRenderPassBeginInfo renderBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        renderBegin.renderPass = renderPass_;
        renderBegin.framebuffer = slot.framebuffer;
        renderBegin.renderArea.extent = {width_, height_};
        renderBegin.clearValueCount = static_cast<uint32_t>(clears.size());
        renderBegin.pClearValues = clears.data();
        vkCmdBeginRenderPass(slot.commandBuffer, &renderBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(slot.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        const VkDeviceSize vertexOffset = 0;
        vkCmdBindVertexBuffers(slot.commandBuffer, 0, 1, &vertexBuffer_, &vertexOffset);
        vkCmdBindIndexBuffer(slot.commandBuffer, indexBuffer_, 0, VK_INDEX_TYPE_UINT32);

        const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
        const Mat4 projection = perspective(45.0f * kDegreesToRadians, aspect, 0.1f, 30.0f);
        const Mat4 view = multiply(
            translation(0.0f, -0.35f, -9.0f),
            multiply(rotationX(20.0f * kDegreesToRadians),
                     rotationY(30.0f * kDegreesToRadians)));
        const float gearAngle = timeSeconds * 70.0f * kDegreesToRadians;
        for (const auto& gear : gears_) {
            const Mat4 model = multiply(translation(gear.x, gear.y, 0.0f),
                                        rotationZ(gearAngle * gear.speed + gear.phase));
            const PushConstants push{multiply(projection, multiply(view, model)), model};
            vkCmdPushConstants(slot.commandBuffer, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(push), &push);
            vkCmdDrawIndexed(slot.commandBuffer, gear.indexCount, 1, gear.firstIndex, 0, 0);
        }
        vkCmdEndRenderPass(slot.commandBuffer);
        if (vkEndCommandBuffer(slot.commandBuffer) != VK_SUCCESS) return false;

        VkSubmitInfo renderSubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        renderSubmit.commandBufferCount = 1;
        renderSubmit.pCommandBuffers = &slot.commandBuffer;
        renderSubmit.signalSemaphoreCount = 1;
        renderSubmit.pSignalSemaphores = &slot.renderComplete;
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        VkSubmitInfo completionSubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        completionSubmit.waitSemaphoreCount = 1;
        completionSubmit.pWaitSemaphores = &slot.renderComplete;
        completionSubmit.pWaitDstStageMask = &waitStage;
        const auto submitStart = Clock::now();
        const std::array<VkSubmitInfo, 2> submits{renderSubmit, completionSubmit};
        VkResult result = vkQueueSubmit(queue_, static_cast<uint32_t>(submits.size()),
                                        submits.data(), slot.renderFence);
        submitMicroseconds = std::chrono::duration<double, std::micro>(
            Clock::now() - submitStart).count();
        if (result != VK_SUCCESS) {
            log_.vulkanError("vkQueueSubmit", result);
            return false;
        }
        if (!geometryUploaded_) {
            geometryUploaded_ = true;
            vertexUpload_.clear();
            indexUpload_.clear();
        }
        slot.gpuSequence = nextGpuSequence_++;
        slot.gpuPending = true;
        return true;
    }

private:
    bool createDevice() {
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "LCL Vulkan Gears";
        application.applicationVersion = 1;
        application.pEngineName = "LCL";
        application.engineVersion = 1;
        application.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &application;
        VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance_);
        if (result != VK_SUCCESS) return fail("vkCreateInstance", result);
        uint32_t count = 0;
        result = vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        if (result != VK_SUCCESS || count == 0) return fail("vkEnumeratePhysicalDevices", result);
        std::vector<VkPhysicalDevice> devices(count);
        result = vkEnumeratePhysicalDevices(instance_, &count, devices.data());
        if (result != VK_SUCCESS) return fail("vkEnumeratePhysicalDevices", result);
        physicalDevice_ = devices.front();
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount, families.data());
        for (uint32_t index = 0; index < familyCount; ++index) {
            if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                queueFamily_ = index;
                break;
            }
        }
        if (queueFamily_ == UINT32_MAX) return false;
        constexpr float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = queueFamily_;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        result = vkCreateDevice(physicalDevice_, &deviceInfo, nullptr, &device_);
        if (result != VK_SUCCESS) return fail("vkCreateDevice", result);
        vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
        return queue_ != VK_NULL_HANDLE;
    }

    bool createBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage,
                      VkBuffer& buffer, VkDeviceMemory& memory) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = bytes;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult result = vkCreateBuffer(device_, &info, nullptr, &buffer);
        if (result != VK_SUCCESS) return fail("vkCreateBuffer", result);
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, buffer, &requirements);
        uint32_t type = findMemoryType(physicalDevice_, requirements.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) {
            type = findMemoryType(physicalDevice_, requirements.memoryTypeBits, 0);
        }
        if (type == UINT32_MAX) return stageFailure("buffer_memory_type_missing");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        result = vkAllocateMemory(device_, &allocation, nullptr, &memory);
        if (result != VK_SUCCESS) return fail("vkAllocateMemory", result);
        result = vkBindBufferMemory(device_, buffer, memory, 0);
        if (result != VK_SUCCESS) return fail("vkBindBufferMemory", result);
        return true;
    }

    bool createGeometry() {
        // Match the classic gears demo proportions. The center distances are
        // the neighboring pitch-radius sums, so the teeth visibly mesh rather
        // than rotating as three unrelated wheels.
        gears_.push_back(appendGear(
            vertexUpload_, indexUpload_, 20,
            1.0f * kGearScale, 3.65f * kGearScale, 4.35f * kGearScale,
            1.0f * kGearScale, {0.80f, 0.10f, 0.00f},
            -3.0f * kGearScale, -2.0f * kGearScale, 1.0f, 0.0f));
        gears_.push_back(appendGear(
            vertexUpload_, indexUpload_, 10,
            0.5f * kGearScale, 1.65f * kGearScale, 2.35f * kGearScale,
            2.0f * kGearScale, {0.00f, 0.80f, 0.20f},
            3.1f * kGearScale, -2.0f * kGearScale, -2.0f,
            -9.0f * kDegreesToRadians));
        gears_.push_back(appendGear(
            vertexUpload_, indexUpload_, 10,
            1.3f * kGearScale, 1.75f * kGearScale, 2.25f * kGearScale,
            0.5f * kGearScale, {0.20f, 0.20f, 1.00f},
            -3.1f * kGearScale, 4.2f * kGearScale, -2.0f,
            -25.0f * kDegreesToRadians));
        return createBuffer(vertexUpload_.size() * sizeof(Vertex),
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            vertexBuffer_, vertexMemory_) &&
               createBuffer(indexUpload_.size() * sizeof(uint32_t),
                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            indexBuffer_, indexMemory_);
    }

    bool createRenderPass() {
        constexpr std::array<VkFormat, 3> candidates{
            VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM};
        depthFormat_ = VK_FORMAT_UNDEFINED;
        for (const VkFormat candidate : candidates) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physicalDevice_, candidate, &properties);
            if ((properties.optimalTilingFeatures &
                 VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
                depthFormat_ = candidate;
                break;
            }
        }
        if (depthFormat_ == VK_FORMAT_UNDEFINED) {
            return stageFailure("depth_format_missing");
        }
        std::array<VkAttachmentDescription, 2> attachments{};
        attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
        attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        attachments[1].format = depthFormat_;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        const VkAttachmentReference colorReference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        const VkAttachmentReference depthReference{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorReference;
        subpass.pDepthStencilAttachment = &depthReference;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask = dependency.srcStageMask;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.attachmentCount = static_cast<uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = 1;
        info.pDependencies = &dependency;
        const VkResult result = vkCreateRenderPass(device_, &info, nullptr, &renderPass_);
        return result == VK_SUCCESS || fail("vkCreateRenderPass", result);
    }

    bool createDepthBuffer() {
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = depthFormat_;
        imageInfo.extent = {width_, height_, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult result = vkCreateImage(device_, &imageInfo, nullptr, &depthImage_);
        if (result != VK_SUCCESS) return fail("vkCreateImage_depth", result);
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, depthImage_, &requirements);
        uint32_t type = findMemoryType(physicalDevice_, requirements.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) type = findMemoryType(physicalDevice_, requirements.memoryTypeBits, 0);
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        result = vkAllocateMemory(device_, &allocation, nullptr, &depthMemory_);
        if (result != VK_SUCCESS) return fail("vkAllocateMemory_depth", result);
        result = vkBindImageMemory(device_, depthImage_, depthMemory_, 0);
        if (result != VK_SUCCESS) return fail("vkBindImageMemory_depth", result);
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = depthImage_;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = depthFormat_;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        result = vkCreateImageView(device_, &viewInfo, nullptr, &depthView_);
        return result == VK_SUCCESS || fail("vkCreateImageView_depth", result);
    }

    bool createTargets() {
        slots_.resize(kBufferCount);
        for (auto& slot : slots_) {
            if (!broker_.createGfxstreamColorBuffer(
                    width_, height_, lcl::gpu_protocol::kAndroidHardwareBufferRgba8888,
                    slot.target)) return false;
            VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            imageInfo.extent = {width_, height_, 1};
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VkResult result = vkCreateImage(device_, &imageInfo, nullptr, &slot.image);
            if (result != VK_SUCCESS) return fail("vkCreateImage_color", result);
            const VkNativeBufferANDROID nativeBuffer{
                .sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID,
                .handle = &slot.target.colorBufferHandle,
                .stride = static_cast<int>(slot.target.stride),
                .format = 1,
            };
            const VkBindImageMemoryInfo bindInfo{
                .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
                .pNext = &nativeBuffer,
                .image = slot.image,
                .memory = VK_NULL_HANDLE,
            };
            result = vkBindImageMemory2(device_, 1, &bindInfo);
            if (result != VK_SUCCESS) return fail("vkBindImageMemory2_native_buffer", result);
            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = slot.image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            result = vkCreateImageView(device_, &viewInfo, nullptr, &slot.view);
            if (result != VK_SUCCESS) return fail("vkCreateImageView_color", result);
            const std::array<VkImageView, 2> attachments{slot.view, depthView_};
            VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebufferInfo.renderPass = renderPass_;
            framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
            framebufferInfo.pAttachments = attachments.data();
            framebufferInfo.width = width_;
            framebufferInfo.height = height_;
            framebufferInfo.layers = 1;
            result = vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &slot.framebuffer);
            if (result != VK_SUCCESS) return fail("vkCreateFramebuffer", result);
        }
        return true;
    }

    bool createPipeline() {
        auto makeShader = [this](const unsigned char* bytes, size_t byteCount,
                                 VkShaderModule& module) {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = byteCount;
            info.pCode = reinterpret_cast<const uint32_t*>(bytes);
            return vkCreateShaderModule(device_, &info, nullptr, &module);
        };
        VkResult result = makeShader(lcl::gears::shaders::kGearVertexShader,
                                     sizeof(lcl::gears::shaders::kGearVertexShader), vertexShader_);
        if (result != VK_SUCCESS) return fail("vkCreateShaderModule_vertex", result);
        result = makeShader(lcl::gears::shaders::kGearFragmentShader,
                            sizeof(lcl::gears::shaders::kGearFragmentShader), fragmentShader_);
        if (result != VK_SUCCESS) return fail("vkCreateShaderModule_fragment", result);
        const VkPushConstantRange pushRange{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants)};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        result = vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_);
        if (result != VK_SUCCESS) return fail("vkCreatePipelineLayout", result);
        const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
             VK_SHADER_STAGE_VERTEX_BIT, vertexShader_, "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
             VK_SHADER_STAGE_FRAGMENT_BIT, fragmentShader_, "main", nullptr},
        }};
        const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const std::array<VkVertexInputAttributeDescription, 3> attributes{{
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
            {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color)},
        }};
        VkPipelineVertexInputStateCreateInfo vertexInput{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();
        VkPipelineInputAssemblyStateCreateInfo assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        const VkViewport viewport{0.0f, 0.0f, static_cast<float>(width_),
                                  static_cast<float>(height_), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, {width_, height_}};
        VkPipelineViewportStateCreateInfo viewportState{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo raster{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;
        VkGraphicsPipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &assembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depth;
        pipelineInfo.pColorBlendState = &blend;
        pipelineInfo.layout = pipelineLayout_;
        pipelineInfo.renderPass = renderPass_;
        result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo,
                                           nullptr, &pipeline_);
        return result == VK_SUCCESS || fail("vkCreateGraphicsPipelines", result);
    }

    bool createCommands() {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily_;
        VkResult result = vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_);
        if (result != VK_SUCCESS) return fail("vkCreateCommandPool", result);
        std::vector<VkCommandBuffer> commandBuffers(slots_.size());
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = commandPool_;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());
        result = vkAllocateCommandBuffers(device_, &commandInfo, commandBuffers.data());
        if (result != VK_SUCCESS) return fail("vkAllocateCommandBuffers", result);
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            auto& slot = slots_[index];
            slot.commandBuffer = commandBuffers[index];
            result = vkCreateFence(device_, &fenceInfo, nullptr, &slot.renderFence);
            if (result != VK_SUCCESS) return fail("vkCreateFence", result);
            result = vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &slot.renderComplete);
            if (result != VK_SUCCESS) return fail("vkCreateSemaphore", result);
        }
        return true;
    }

    bool fail(std::string_view operation, VkResult result) {
        log_.vulkanError(operation, result);
        return false;
    }

    bool stageFailure(std::string_view stage) {
        log_.stage(stage);
        return false;
    }

    void destroy() {
        if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
        if (device_ != VK_NULL_HANDLE) {
            if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
            if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
            if (vertexShader_) vkDestroyShaderModule(device_, vertexShader_, nullptr);
            if (fragmentShader_) vkDestroyShaderModule(device_, fragmentShader_, nullptr);
            for (auto& slot : slots_) {
                if (slot.renderComplete) {
                    vkDestroySemaphore(device_, slot.renderComplete, nullptr);
                }
                if (slot.renderFence) vkDestroyFence(device_, slot.renderFence, nullptr);
                if (slot.framebuffer) vkDestroyFramebuffer(device_, slot.framebuffer, nullptr);
                if (slot.view) vkDestroyImageView(device_, slot.view, nullptr);
                if (slot.image) vkDestroyImage(device_, slot.image, nullptr);
                if (slot.target.targetId) broker_.destroyGfxstreamColorBuffer(slot.target.targetId);
            }
            if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
            if (depthView_) vkDestroyImageView(device_, depthView_, nullptr);
            if (depthImage_) vkDestroyImage(device_, depthImage_, nullptr);
            if (depthMemory_) vkFreeMemory(device_, depthMemory_, nullptr);
            if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
            if (vertexBuffer_) vkDestroyBuffer(device_, vertexBuffer_, nullptr);
            if (vertexMemory_) vkFreeMemory(device_, vertexMemory_, nullptr);
            if (indexBuffer_) vkDestroyBuffer(device_, indexBuffer_, nullptr);
            if (indexMemory_) vkFreeMemory(device_, indexMemory_, nullptr);
            vkDestroyDevice(device_, nullptr);
        }
        if (instance_) vkDestroyInstance(instance_, nullptr);
    }

    lcl::gpu::GpuClient& broker_;
    const RunLog& log_;
    uint32_t width_{0};
    uint32_t height_{0};
    VkInstance instance_{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue queue_{VK_NULL_HANDLE};
    uint32_t queueFamily_{UINT32_MAX};
    VkBuffer vertexBuffer_{VK_NULL_HANDLE};
    VkDeviceMemory vertexMemory_{VK_NULL_HANDLE};
    VkBuffer indexBuffer_{VK_NULL_HANDLE};
    VkDeviceMemory indexMemory_{VK_NULL_HANDLE};
    VkFormat depthFormat_{VK_FORMAT_D32_SFLOAT};
    VkImage depthImage_{VK_NULL_HANDLE};
    VkDeviceMemory depthMemory_{VK_NULL_HANDLE};
    VkImageView depthView_{VK_NULL_HANDLE};
    VkRenderPass renderPass_{VK_NULL_HANDLE};
    VkShaderModule vertexShader_{VK_NULL_HANDLE};
    VkShaderModule fragmentShader_{VK_NULL_HANDLE};
    VkPipelineLayout pipelineLayout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    VkCommandPool commandPool_{VK_NULL_HANDLE};
    uint64_t nextGpuSequence_{1};
    std::vector<PresentSlot> slots_;
    std::vector<GearRange> gears_;
    std::vector<Vertex> vertexUpload_;
    std::vector<uint32_t> indexUpload_;
    bool geometryUploaded_{false};
};

struct Metrics {
    uint64_t submitted{0};
    uint64_t presented{0};
    uint64_t dropped{0};
    uint64_t rejected{0};
    double submitMicroseconds{0.0};
    double presentMilliseconds{0.0};
    double frameMilliseconds{0.0};
    Clock::time_point start{};
    Clock::time_point lastPresented{};
};

} // namespace

int main() {
    RunLog log;
    if (setenv("VK_ICD_FILENAMES", kIcdPath, 1) != 0) return 10;
    log.stage("loader_configured");
    lcl::gpu::GpuClient broker;
    if (!completeHandshake(broker, log)) {
        log.stage("gpu_broker_handshake_failed");
        return 11;
    }

    lcl::client::SurfaceOptions options{};
    options.surfaceId = 1;
    options.appId = "org.lcl.vulkan-gears";
    options.title = "Vulkan Gears";
    options.bounds = {48.0f, 72.0f, 640.0f, 720.0f};
    readEnvironmentUint64("LCL_LAUNCH_TOKEN", options.launchToken);
    readEnvironmentUint64("LCL_APP_INSTANCE_ID", options.appInstanceId);
    unsetenv("LCL_LAUNCH_TOKEN");
    unsetenv("LCL_APP_INSTANCE_ID");
    lcl::client::SurfaceClient surface;
    if (!surface.connect(options)) {
        log.stage("surface_connect_failed");
        return 12;
    }
    const auto configureDeadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < configureDeadline &&
           (!surface.hasConfigure() || surface.rasterFd() < 0)) {
        surface.dispatch();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!surface.hasConfigure() || surface.rasterFd() < 0) {
        log.stage("surface_configure_timeout");
        return 13;
    }
    const lcl::client::ConfigureEvent configure = surface.configure();
    const double scaledWidth =
        std::ceil(static_cast<double>(configure.bounds.width) * configure.bufferScale);
    const double scaledHeight =
        std::ceil(static_cast<double>(configure.bounds.height) * configure.bufferScale);
    if (!std::isfinite(scaledWidth) || !std::isfinite(scaledHeight) ||
        scaledWidth < 1.0 || scaledHeight < 1.0 || scaledWidth > UINT32_MAX ||
        scaledHeight > UINT32_MAX) {
        log.stage("surface_extent_invalid");
        return 14;
    }
    const uint32_t width = static_cast<uint32_t>(scaledWidth);
    const uint32_t height = static_cast<uint32_t>(scaledHeight);
    VulkanRenderer renderer(broker, log);
    if (!renderer.initialize(width, height)) return 15;

    Metrics metrics{};
    metrics.start = Clock::now();
    std::optional<Clock::time_point> pendingPresent;
    bool stableLogged = false;
    bool running = true;
    log.stage("presentation_feedback_vsync_active");
    log.stage("render_loop_started");

    while (running && surface.connected()) {
        for (auto& event : surface.dispatch()) {
            if (auto* presented = std::get_if<lcl::client::FramePresentedEvent>(&event)) {
                (void)presented;
                const auto now = Clock::now();
                ++metrics.presented;
                if (pendingPresent) {
                    metrics.presentMilliseconds +=
                        std::chrono::duration<double, std::milli>(now - *pendingPresent).count();
                    pendingPresent.reset();
                }
                if (metrics.lastPresented != Clock::time_point{}) {
                    metrics.frameMilliseconds +=
                        std::chrono::duration<double, std::milli>(now - metrics.lastPresented).count();
                }
                metrics.lastPresented = now;
                if (metrics.presented % 60 == 0) {
                    const double elapsed = Seconds(now - metrics.start).count();
                    const double fps = elapsed > 0.0 ? metrics.presented / elapsed : 0.0;
                    const double averageFrame = metrics.presented > 1
                        ? metrics.frameMilliseconds / static_cast<double>(metrics.presented - 1)
                        : 0.0;
                    log.metrics(metrics.submitted, metrics.presented, fps, averageFrame,
                                metrics.submitMicroseconds / metrics.submitted,
                                metrics.presentMilliseconds / metrics.presented,
                                metrics.dropped, metrics.rejected);
                }
                if (!stableLogged && metrics.presented >= 300) {
                    log.stage("stable_300_frames");
                    stableLogged = true;
                }
            } else if (auto* discarded = std::get_if<lcl::client::FrameDiscardedEvent>(&event)) {
                ++metrics.dropped;
                if (discarded->reason == lcl::raster_protocol::DiscardReason::InvalidFrame) {
                    ++metrics.rejected;
                }
                pendingPresent.reset();
            } else if (auto* released = std::get_if<lcl::client::BufferReleasedEvent>(&event)) {
                PresentSlot* slot = renderer.slotFor(released->bufferId);
                if (!slot) continue;
                if (slot->releasePending) {
                    log.syncError(released->bufferId);
                    ++metrics.rejected;
                    running = false;
                    break;
                }
                if (released->reason ==
                    lcl::raster_protocol::ExternalBufferReleaseReason::Rejected) {
                    ++metrics.rejected;
                }
                slot->releaseFence = std::move(released->releaseFence);
                slot->releasePending = true;
            } else if (std::get_if<lcl::client::SurfaceClosedEvent>(&event)) {
                running = false;
                break;
            } else if (auto* updated = std::get_if<lcl::client::ConfigureEvent>(&event)) {
                const uint32_t newWidth = static_cast<uint32_t>(std::ceil(
                    static_cast<double>(updated->bounds.width) * updated->bufferScale));
                const uint32_t newHeight = static_cast<uint32_t>(std::ceil(
                    static_cast<double>(updated->bounds.height) * updated->bufferScale));
                if (newWidth != width || newHeight != height) {
                    log.stage("resize_requires_restart");
                    running = false;
                    break;
                }
            }
        }

        if (running && !renderer.collectReleasedBuffers()) {
            ++metrics.rejected;
            running = false;
            break;
        }
        if (running && !renderer.collectGpuCompletions()) {
            ++metrics.rejected;
            running = false;
            break;
        }

        if (running && surface.hasFrameCredit()) {
            PresentSlot* slot = renderer.readySlot();
            if (slot) {
                lcl::client::PlatformNativeFrame frame{};
                frame.bufferId = slot->target.targetId;
                frame.contentRevision = slot->revision;
                frame.width = slot->target.width;
                frame.height = slot->target.height;
                frame.stride = slot->target.stride * sizeof(uint32_t);
                frame.format = lcl::gpu_protocol::kAndroidHardwareBufferRgba8888;
                frame.damage = {0.0f, 0.0f, configure.bounds.width, configure.bounds.height};
                frame.opaque = true;
                frame.writeHandle = [&broker, id = slot->target.targetId](int sidebandFd) {
                    return broker.deliverNativeBuffer(id, sidebandFd);
                };
                const auto presentStart = Clock::now();
                if (!surface.submitFrame(std::move(frame))) {
                    ++metrics.rejected;
                    log.stage("surface_submit_failed");
                    break;
                }
                slot->busy = true;
                slot->readyToPresent = false;
                pendingPresent = presentStart;
            }
        }

        const auto now = Clock::now();
        PresentSlot* slot = renderer.availableSlot();
        if (running && slot && renderer.renderAheadCount() < kRenderAheadCount) {
            double submitMicroseconds = 0.0;
            const float animationSeconds = static_cast<float>(Seconds(now - metrics.start).count());
            if (!renderer.render(*slot, animationSeconds, submitMicroseconds)) {
                log.stage("render_failed");
                break;
            }
            ++metrics.submitted;
            metrics.submitMicroseconds += submitMicroseconds;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    log.stage(stableLogged ? "complete_after_300_frames" : "stopped_before_300_frames");
    return stableLogged ? 0 : 16;
}
