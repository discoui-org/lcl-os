#include "lcl-gpu/gpu_client.hpp"

#include <vulkan/vk_icd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <poll.h>

struct VkPhysicalDevice_T;
struct VkDevice_T;
struct VkQueue_T;

struct VkInstance_T {
    VK_LOADER_DATA loaderData{};
    lcl::gpu::DeviceCapabilities capabilities{};
    VkPhysicalDevice_T* physicalDevice{nullptr};
};
struct VkPhysicalDevice_T {
    VK_LOADER_DATA loaderData{};
    VkInstance_T* instance{nullptr};
};
struct VkDevice_T {
    VK_LOADER_DATA loaderData{};
    VkPhysicalDevice_T* physicalDevice{nullptr};
    VkQueue_T* queue{nullptr};
};
struct VkQueue_T {
    VK_LOADER_DATA loaderData{};
    VkDevice_T* device{nullptr};
};

namespace {

constexpr uint32_t kIcdInterfaceVersion = 5;

bool queryBroker(lcl::gpu::DeviceCapabilities& capabilities) {
    const char* configuredSocket = std::getenv("LCL_GPU_SOCKET");
    lcl::gpu::GpuClient client;
    if (!client.connect(configuredSocket ? configuredSocket : "/Runtime/lcl-gpu.sock") ||
        !client.beginHandshake(1)) return false;
    pollfd ready{client.fd(), POLLIN, 0};
    int result = -1;
    do result = poll(&ready, 1, 1'000); while (result < 0 && errno == EINTR);
    return result == 1 && (ready.revents & POLLIN) != 0 &&
           client.dispatch(capabilities) == lcl::gpu::HandshakeStatus::Ready;
}

bool hasName(const char* name, const char* expected) {
    return name && std::strcmp(name, expected) == 0;
}

} // namespace

extern "C" {

VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* version) {
    if (!version || *version == 0) return VK_ERROR_INITIALIZATION_FAILED;
    *version = std::min(*version, kIcdInterfaceVersion);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t* version) {
    if (!version) return VK_ERROR_INITIALIZATION_FAILED;
    *version = VK_API_VERSION_1_1;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
        const char*, uint32_t* count, VkExtensionProperties*) {
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    *count = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(
        uint32_t* count, VkLayerProperties*) {
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    *count = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* create,
                                                  const VkAllocationCallbacks*, VkInstance* out) {
    if (!create || !out || create->sType != VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO ||
        create->enabledExtensionCount != 0 || create->enabledLayerCount != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    auto* instance = new VkInstance_T;
    if (!queryBroker(instance->capabilities)) {
        delete instance;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    instance->physicalDevice = new VkPhysicalDevice_T{.instance = instance};
    set_loader_magic_value(instance);
    set_loader_magic_value(instance->physicalDevice);
    *out = instance;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance,
                                              const VkAllocationCallbacks*) {
    if (!instance) return;
    delete instance->physicalDevice;
    delete instance;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance,
                                                            uint32_t* count,
                                                            VkPhysicalDevice* devices) {
    if (!instance || !count) return VK_ERROR_INITIALIZATION_FAILED;
    if (!devices) {
        *count = 1;
        return VK_SUCCESS;
    }
    if (*count == 0) return VK_INCOMPLETE;
    devices[0] = instance->physicalDevice;
    *count = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(
        VkPhysicalDevice physical, VkPhysicalDeviceProperties* properties) {
    if (!physical || !properties) return;
    *properties = {};
    const auto& caps = physical->instance->capabilities;
    properties->apiVersion = std::min(caps.vulkanApiVersion, VK_API_VERSION_1_1);
    properties->driverVersion = VK_MAKE_VERSION(0, 1, 0);
    properties->vendorID = caps.vendorId;
    properties->deviceID = caps.deviceId;
    properties->deviceType = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    properties->limits.maxImageDimension2D = caps.maxImageDimension2D;
    std::strncpy(properties->deviceName, caps.deviceName.c_str(),
                 VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice,
                                                        VkPhysicalDeviceFeatures* features) {
    if (features) *features = {};
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(
        VkPhysicalDevice physical, uint32_t* count, VkQueueFamilyProperties* properties) {
    if (!physical || !count) return;
    if (!properties) {
        *count = 1;
        return;
    }
    if (*count == 0) return;
    properties[0] = {.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT |
                                    VK_QUEUE_TRANSFER_BIT,
                     .queueCount = 1,
                     .timestampValidBits = 0,
                     .minImageTransferGranularity = {1, 1, 1}};
    *count = 1;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(
        VkPhysicalDevice physical, VkPhysicalDeviceMemoryProperties* properties) {
    if (!physical || !properties) return;
    *properties = {};
    properties->memoryTypeCount = 1;
    properties->memoryTypes[0] = {.propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                  .heapIndex = 0};
    properties->memoryHeapCount = 1;
    properties->memoryHeaps[0] = {.size = 512ull * 1024ull * 1024ull,
                                  .flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(
        VkPhysicalDevice, VkFormat, VkImageType, VkImageTiling, VkImageUsageFlags,
        VkImageCreateFlags, VkImageFormatProperties*) {
    return VK_ERROR_FORMAT_NOT_SUPPORTED;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(
        VkPhysicalDevice, VkFormat, VkFormatProperties* properties) {
    if (properties) *properties = {};
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties(
        VkPhysicalDevice, VkFormat, VkImageType, VkSampleCountFlagBits,
        VkImageUsageFlags, VkImageTiling, uint32_t* count,
        VkSparseImageFormatProperties*) {
    if (count) *count = 0;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(
        VkPhysicalDevice physical, const char*, uint32_t* count, VkExtensionProperties*) {
    if (!physical || !count) return VK_ERROR_INITIALIZATION_FAILED;
    *count = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physical,
                                                const VkDeviceCreateInfo* create,
                                                const VkAllocationCallbacks*, VkDevice* out) {
    if (!physical || !create || !out || create->sType != VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO ||
        create->enabledExtensionCount != 0 || create->queueCreateInfoCount != 1 ||
        create->pQueueCreateInfos[0].queueFamilyIndex != 0 ||
        create->pQueueCreateInfos[0].queueCount != 1) return VK_ERROR_INITIALIZATION_FAILED;
    auto* device = new VkDevice_T{.physicalDevice = physical};
    device->queue = new VkQueue_T{.device = device};
    set_loader_magic_value(device);
    set_loader_magic_value(device->queue);
    *out = device;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks*) {
    if (!device) return;
    delete device->queue;
    delete device;
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device, uint32_t family,
                                              uint32_t index, VkQueue* queue) {
    if (queue) *queue = device && family == 0 && index == 0 ? device->queue : VK_NULL_HANDLE;
}

VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device) {
    return device ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue) {
    return queue ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char* name) {
    if (hasName(name, "vkDestroyDevice")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDevice);
    if (hasName(name, "vkGetDeviceQueue")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue);
    if (hasName(name, "vkDeviceWaitIdle")) return reinterpret_cast<PFN_vkVoidFunction>(vkDeviceWaitIdle);
    if (hasName(name, "vkQueueWaitIdle")) return reinterpret_cast<PFN_vkVoidFunction>(vkQueueWaitIdle);
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char* name) {
    if (hasName(name, "vkGetInstanceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
    if (hasName(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    if (hasName(name, "vkCreateInstance")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateInstance);
    if (hasName(name, "vkDestroyInstance")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyInstance);
    if (hasName(name, "vkEnumerateInstanceVersion")) return reinterpret_cast<PFN_vkVoidFunction>(vkEnumerateInstanceVersion);
    if (hasName(name, "vkEnumerateInstanceExtensionProperties")) return reinterpret_cast<PFN_vkVoidFunction>(vkEnumerateInstanceExtensionProperties);
    if (hasName(name, "vkEnumerateInstanceLayerProperties")) return reinterpret_cast<PFN_vkVoidFunction>(vkEnumerateInstanceLayerProperties);
    if (hasName(name, "vkEnumeratePhysicalDevices")) return reinterpret_cast<PFN_vkVoidFunction>(vkEnumeratePhysicalDevices);
    if (hasName(name, "vkGetPhysicalDeviceProperties")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceProperties);
    if (hasName(name, "vkGetPhysicalDeviceFeatures")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceFeatures);
    if (hasName(name, "vkGetPhysicalDeviceQueueFamilyProperties")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceQueueFamilyProperties);
    if (hasName(name, "vkGetPhysicalDeviceMemoryProperties")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceMemoryProperties);
    if (hasName(name, "vkGetPhysicalDeviceImageFormatProperties")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceImageFormatProperties);
    if (hasName(name, "vkGetPhysicalDeviceFormatProperties")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceFormatProperties);
    if (hasName(name, "vkGetPhysicalDeviceSparseImageFormatProperties")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceSparseImageFormatProperties);
    if (hasName(name, "vkEnumerateDeviceExtensionProperties")) return reinterpret_cast<PFN_vkVoidFunction>(vkEnumerateDeviceExtensionProperties);
    if (hasName(name, "vkCreateDevice")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateDevice);
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,
                                                                     const char* name) {
    return vkGetInstanceProcAddr(instance, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(
        VkInstance, const char* name) {
    return vkGetInstanceProcAddr(VK_NULL_HANDLE, name);
}

} // extern "C"
