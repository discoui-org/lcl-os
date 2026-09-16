#include "system/ipc/gpu_protocol.hpp"
#include "system/security/session_user.hpp"
#include "system/gpu/vulkan_clear_renderer.hpp"

#if LCL_HAS_GFXSTREAM_ANDROID_HOST
#include "lcl-gfxstream/android_host_ingress.hpp"
#endif

#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace {

using lcl::gpu_protocol::DeviceInfo;

bool setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool hasExtension(const std::vector<VkExtensionProperties>& extensions,
                  const char* name) {
    for (const auto& extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) return true;
    }
    return false;
}

std::optional<DeviceInfo> queryDevice() {
    uint32_t supportedApiVersion = VK_API_VERSION_1_0;
    const auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    if (enumerateVersion && enumerateVersion(&supportedApiVersion) != VK_SUCCESS) {
        return std::nullopt;
    }

    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "lcl-gpud";
    application.applicationVersion = 1;
    application.pEngineName = "lcl";
    application.engineVersion = 1;
    application.apiVersion = std::min(supportedApiVersion, VK_API_VERSION_1_1);

    VkInstanceCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create.pApplicationInfo = &application;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&create, nullptr, &instance) != VK_SUCCESS) {
        return std::nullopt;
    }

    uint32_t count = 0;
    const VkResult enumerated = vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (enumerated != VK_SUCCESS || count == 0) {
        vkDestroyInstance(instance, nullptr);
        return std::nullopt;
    }
    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) != VK_SUCCESS) {
        vkDestroyInstance(instance, nullptr);
        return std::nullopt;
    }

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(devices.front(), &properties);
    uint32_t extensionCount = 0;
    if (vkEnumerateDeviceExtensionProperties(devices.front(), nullptr,
                                              &extensionCount, nullptr) != VK_SUCCESS) {
        vkDestroyInstance(instance, nullptr);
        return std::nullopt;
    }
    std::vector<VkExtensionProperties> extensions(extensionCount);
    if (extensionCount != 0 &&
        vkEnumerateDeviceExtensionProperties(devices.front(), nullptr,
                                             &extensionCount, extensions.data()) != VK_SUCCESS) {
        vkDestroyInstance(instance, nullptr);
        return std::nullopt;
    }

    DeviceInfo info{};
    info.vulkanApiVersion = properties.apiVersion;
    info.vendorId = properties.vendorID;
    info.deviceId = properties.deviceID;
    info.maxImageDimension2D = properties.limits.maxImageDimension2D;
    if (hasExtension(extensions, VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME)) {
        info.flags |= lcl::gpu_protocol::kDeviceSupportsAndroidHardwareBuffer;
    }
    if (hasExtension(extensions, VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME)) {
        info.flags |= lcl::gpu_protocol::kDeviceSupportsExternalFenceFd;
    }
    std::strncpy(info.deviceName, properties.deviceName,
                 sizeof(info.deviceName) - 1);
    vkDestroyInstance(instance, nullptr);
    return info;
}

int createListener(const std::string& path) {
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) return -1;
    const int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (listener < 0) return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    unlink(path.c_str());
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        chown(path.c_str(), lcl::security::kSessionUserUid,
              lcl::security::kApplicationRuntimeGid) != 0 ||
        chmod(path.c_str(), 0660) != 0 || listen(listener, 16) != 0 ||
        !setNonBlocking(listener)) {
        close(listener);
        unlink(path.c_str());
        return -1;
    }
    return listener;
}

void sendError(int client, uint32_t code, const char* message) {
    lcl::gpu_protocol::Error error{};
    error.code = code;
    std::strncpy(error.message, message, sizeof(error.message) - 1);
    (void)lcl::gpu_protocol::sendPacket(client, lcl::gpu_protocol::Opcode::Error,
                                        &error, sizeof(error));
}

void handleClient(int client, const DeviceInfo& device,
                  lcl::gpu::VulkanClearRenderer& renderer
#if LCL_HAS_GFXSTREAM_ANDROID_HOST
                  , lcl::gfxstream::AndroidHostIngress* gfxstreamIngress
#endif
                  ) {
    const char* traceFrameSetting = std::getenv("LCL_GPU_TRACE_FRAMES");
    const bool traceFrames = traceFrameSetting && traceFrameSetting[0] == '1';
    // Keep brokered gfxstream targets disjoint from ClearColor's legacy
    // buffer IDs; both still use the same authorized raster sideband method.
    uint64_t nextGfxstreamTargetId = uint64_t{1} << 63;
#if LCL_HAS_GFXSTREAM_ANDROID_HOST
    std::unordered_map<uint64_t, uint32_t> gfxstreamTargets;
    const auto destroyGfxstreamTargets = [&]() noexcept {
        if (!gfxstreamIngress) return;
        for (const auto& [_, colorBufferHandle] : gfxstreamTargets) {
            gfxstreamIngress->destroyColorBuffer(colorBufferHandle);
        }
        gfxstreamTargets.clear();
    };
#endif
    lcl::gpu_protocol::Header header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    const auto received = lcl::gpu_protocol::receivePacketWithFd(
        client, header, payload, receivedFd);
    if (received != lcl::gpu_protocol::ReceiveStatus::Received || receivedFd >= 0) {
        if (receivedFd >= 0) close(receivedFd);
        return;
    }
    const auto* hello = lcl::gpu_protocol::payloadAs<lcl::gpu_protocol::Hello>(
        header, payload, lcl::gpu_protocol::Opcode::Hello);
    if (!hello || hello->requestedVersion != lcl::gpu_protocol::kVersion ||
        hello->flags != 0) {
        std::cerr << "[LCL Gpud] rejected invalid client hello\n";
        sendError(client, 1, "unsupported lcl-gpu client");
        return;
    }
    std::cerr << "[LCL Gpud] client handshake accepted\n";
    (void)lcl::gpu_protocol::sendPacket(client, lcl::gpu_protocol::Opcode::DeviceInfo,
                                        &device, sizeof(device));
    for (;;) {
        receivedFd = -1;
        payload.clear();
        const auto status = lcl::gpu_protocol::receivePacketWithFd(
            client, header, payload, receivedFd);
        if (status == lcl::gpu_protocol::ReceiveStatus::WouldBlock) {
            pollfd ready{client, POLLIN, 0};
            int pollResult = -1;
            do pollResult = poll(&ready, 1, 1'000); while (pollResult < 0 && errno == EINTR);
            if (pollResult > 0) continue;
            if (pollResult == 0) continue;
            break;
        }
        if (status != lcl::gpu_protocol::ReceiveStatus::Received) {
            if (receivedFd >= 0) close(receivedFd);
            break;
        }
        if (const auto* clear = lcl::gpu_protocol::payloadAs<lcl::gpu_protocol::ClearColor>(
                header, payload, lcl::gpu_protocol::Opcode::ClearColor)) {
            if (receivedFd >= 0) {
                close(receivedFd);
                sendError(client, 2, "clear command must not carry an FD");
                break;
            }
            int acquireFence = -1;
            const auto ready = renderer.clear(*clear, acquireFence);
            if (!ready || !lcl::gpu_protocol::sendPacketWithFd(
                    client, lcl::gpu_protocol::Opcode::ClearColorReady,
                    &*ready, sizeof(*ready), acquireFence)) {
                if (acquireFence >= 0) close(acquireFence);
                sendError(client, 3, "native Vulkan clear failed");
                break;
            }
            std::cerr << "[LCL Gpud] cleared buffer " << ready->bufferId << "\n";
            if (acquireFence >= 0) close(acquireFence);
        } else if (const auto* delivery =
                       lcl::gpu_protocol::payloadAs<lcl::gpu_protocol::DeliverNativeBuffer>(
                           header, payload, lcl::gpu_protocol::Opcode::DeliverNativeBuffer)) {
            bool delivered = false;
#if LCL_HAS_GFXSTREAM_ANDROID_HOST
            if (const auto target = gfxstreamTargets.find(delivery->bufferId);
                target != gfxstreamTargets.end()) {
                delivered = receivedFd >= 0 &&
                    gfxstreamIngress->deliverColorBuffer(target->second, receivedFd);
            } else
#endif
            {
                delivered = receivedFd >= 0 && renderer.deliver(delivery->bufferId, receivedFd);
            }
            if (receivedFd >= 0) close(receivedFd);
            lcl::gpu_protocol::DeliveryComplete complete{};
            complete.bufferId = delivery->bufferId;
            complete.status = delivered ? 0 : 1;
            if (!lcl::gpu_protocol::sendPacket(client,
                                                lcl::gpu_protocol::Opcode::DeliveryComplete,
                                                &complete, sizeof(complete))) break;
            if (!delivered) break;
            if (traceFrames) {
                std::cerr << "[LCL Gpud] delivered buffer " << delivery->bufferId << "\n";
            }
        } else if (const auto* release =
                       lcl::gpu_protocol::payloadAs<lcl::gpu_protocol::ReleasePresentedBuffer>(
                           header, payload, lcl::gpu_protocol::Opcode::ReleasePresentedBuffer)) {
            bool isGfxstreamTarget = false;
#if LCL_HAS_GFXSTREAM_ANDROID_HOST
            isGfxstreamTarget = gfxstreamTargets.contains(release->bufferId);
#endif
            if (isGfxstreamTarget) {
                // The compositor owns this release fence. The PoC waits for
                // guest submit before delivery, so gpud need only consume it.
                if (receivedFd >= 0) close(receivedFd);
            } else {
                renderer.release(release->bufferId, receivedFd);
                receivedFd = -1;
            }
            if (traceFrames) {
                std::cerr << "[LCL Gpud] released buffer " << release->bufferId << "\n";
            }
        } else if (const auto* open =
                       lcl::gpu_protocol::payloadAs<lcl::gpu_protocol::OpenGfxstreamStream>(
                           header, payload, lcl::gpu_protocol::Opcode::OpenGfxstreamStream)) {
            if (receivedFd >= 0) close(receivedFd);
            if (open->transportVersion != lcl::gpu_protocol::kGfxstreamTransportVersion ||
                open->flags != 0) {
                sendError(client, 5, "unsupported gfxstream transport version");
#if LCL_HAS_GFXSTREAM_ANDROID_HOST
            } else if (!gfxstreamIngress) {
                sendError(client, 6, "gfxstream decoder unavailable");
            } else {
                std::string error;
                if (!gfxstreamIngress->initialize(error)) {
                    std::cerr << "[LCL Gpud] gfxstream initialization failed: " << error << '\n';
                    sendError(client, 6, "gfxstream decoder unavailable");
                } else {
                    const lcl::gpu_protocol::GfxstreamStreamReady ready{};
                    if (!lcl::gpu_protocol::sendPacket(
                            client, lcl::gpu_protocol::Opcode::GfxstreamStreamReady,
                            &ready, sizeof(ready))) {
                        break;
                    }
                    // From this point on the socket carries only bounded opaque
                    // gfxstream bytes.  The adapter owns the native decoder
                    // lifetime; no LCL Vulkan command protocol is introduced.
                    gfxstreamIngress->serve(client);
                    return;
                }
#else
            } else {
                sendError(client, 6, "gfxstream decoder unavailable");
#endif
            }
            break;
        } else if (const auto* request =
                       lcl::gpu_protocol::payloadAs<
                           lcl::gpu_protocol::CreateGfxstreamColorBuffer>(
                           header, payload,
                           lcl::gpu_protocol::Opcode::CreateGfxstreamColorBuffer)) {
            if (receivedFd >= 0) close(receivedFd);
#if LCL_HAS_GFXSTREAM_ANDROID_HOST
            if (!gfxstreamIngress || request->flags != 0 ||
                request->format != lcl::gpu_protocol::kAndroidHardwareBufferRgba8888 ||
                request->width == 0 || request->height == 0 || request->width > 16'384 ||
                request->height > 16'384) {
                sendError(client, 7, "invalid gfxstream target request");
                break;
            }
            std::string error;
            if (!gfxstreamIngress->initialize(error)) {
                sendError(client, 6, "gfxstream decoder unavailable");
                break;
            }
            const auto colorBuffer = gfxstreamIngress->createColorBuffer(
                request->width, request->height);
            if (!colorBuffer || nextGfxstreamTargetId == 0) {
                sendError(client, 8, "could not allocate gfxstream target");
                break;
            }
            const uint64_t targetId = nextGfxstreamTargetId++;
            gfxstreamTargets.emplace(targetId, colorBuffer->colorBufferHandle);
            const lcl::gpu_protocol::GfxstreamColorBufferReady ready{
                .targetId = targetId,
                .colorBufferHandle = colorBuffer->colorBufferHandle,
                .width = request->width,
                .height = request->height,
                .format = request->format,
                .stride = colorBuffer->stride,
            };
            if (!lcl::gpu_protocol::sendPacket(
                    client, lcl::gpu_protocol::Opcode::GfxstreamColorBufferReady,
                    &ready, sizeof(ready))) {
                gfxstreamIngress->destroyColorBuffer(colorBuffer->colorBufferHandle);
                gfxstreamTargets.erase(targetId);
                break;
            }
#else
            sendError(client, 6, "gfxstream decoder unavailable");
            break;
#endif
        } else if (const auto* destroy =
                       lcl::gpu_protocol::payloadAs<
                           lcl::gpu_protocol::DestroyGfxstreamColorBuffer>(
                           header, payload,
                           lcl::gpu_protocol::Opcode::DestroyGfxstreamColorBuffer)) {
            if (receivedFd >= 0) close(receivedFd);
#if LCL_HAS_GFXSTREAM_ANDROID_HOST
            const auto found = gfxstreamTargets.find(destroy->targetId);
            if (found == gfxstreamTargets.end()) {
                sendError(client, 9, "unknown gfxstream target");
                break;
            }
            gfxstreamIngress->destroyColorBuffer(found->second);
            gfxstreamTargets.erase(found);
#else
            sendError(client, 6, "gfxstream decoder unavailable");
            break;
#endif
        } else {
            if (receivedFd >= 0) close(receivedFd);
            sendError(client, 4, "unsupported gfxstream PoC command");
            break;
        }
    }
#if LCL_HAS_GFXSTREAM_ANDROID_HOST
    destroyGfxstreamTargets();
#endif
    renderer.releaseAll();
}

} // namespace

int main(int argc, char** argv) {
    bool dumpCapabilities = false;
    std::string socketPath = "/Runtime/lcl-gpu.sock";
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--dump-capabilities") {
            dumpCapabilities = true;
        } else if (argument == "--socket" && index + 1 < argc) {
            socketPath = argv[++index];
        } else {
            std::cerr << "usage: lcl-gpud-android [--dump-capabilities] [--socket PATH]\n";
            return 2;
        }
    }

    const auto device = queryDevice();
    if (!device) {
        std::cerr << "[LCL Gpud] Android Vulkan device discovery failed\n";
        return 1;
    }
    if (dumpCapabilities) {
        std::cout << "api=" << VK_VERSION_MAJOR(device->vulkanApiVersion) << '.'
                  << VK_VERSION_MINOR(device->vulkanApiVersion)
                  << " vendor=" << device->vendorId
                  << " device=" << device->deviceId
                  << " maxImage2D=" << device->maxImageDimension2D
                  << " ahb=" << ((device->flags & lcl::gpu_protocol::
                                      kDeviceSupportsAndroidHardwareBuffer) != 0)
                  << " externalFenceFd=" << ((device->flags & lcl::gpu_protocol::
                                                 kDeviceSupportsExternalFenceFd) != 0)
                  << " name=" << device->deviceName << '\n';
        return 0;
    }

    const int listener = createListener(socketPath);
    if (listener < 0) {
        std::cerr << "[LCL Gpud] Cannot listen at " << socketPath << ": "
                  << std::strerror(errno) << '\n';
        return 1;
    }
    lcl::gpu::VulkanClearRenderer renderer;
#if LCL_HAS_GFXSTREAM_ANDROID_HOST
    lcl::gfxstream::AndroidHostIngress gfxstreamIngress;
#endif
    std::cout << "[LCL Gpud] control plane ready at " << socketPath << '\n';
    for (;;) {
        const int client = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(5'000);
                continue;
            }
            break;
        }
        std::cerr << "[LCL Gpud] client connected\n";
        // The control connection deliberately stays alive while a guest ICD
        // uses its separate opaque gfxstream stream.  Serving synchronously
        // would deadlock that second authorized connection at vkCreateInstance.
        std::thread([client, &renderer, device
#if LCL_HAS_GFXSTREAM_ANDROID_HOST
                     , &gfxstreamIngress
#endif
        ]() {
            handleClient(client, *device, renderer
#if LCL_HAS_GFXSTREAM_ANDROID_HOST
                         , &gfxstreamIngress
#endif
            );
            std::cerr << "[LCL Gpud] client disconnected\n";
            close(client);
        }).detach();
    }
    close(listener);
    unlink(socketPath.c_str());
    return 0;
}
