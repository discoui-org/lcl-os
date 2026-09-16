#include <gtest/gtest.h>

#include "system/ipc/gpu_protocol.hpp"

#include <vulkan/vk_icd.h>

#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

class LclVulkanIcdTest : public ::testing::Test {
protected:
    std::string socketPath_;
    std::string icdJsonPath_;
    int listener_{-1};
    std::thread server_;

    void SetUp() override {
        socketPath_ = (std::filesystem::temp_directory_path() /
                       ("lcl_vulkan_icd_" + std::to_string(getpid()) + ".sock")).string();
        listener_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        ASSERT_GE(listener_, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, socketPath_.c_str(), sizeof(address.sun_path) - 1);
        ASSERT_EQ(bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
        ASSERT_EQ(listen(listener_, 1), 0);
        ASSERT_EQ(setenv("LCL_GPU_SOCKET", socketPath_.c_str(), 1), 0);
        icdJsonPath_ = socketPath_ + ".json";
        std::ofstream json(icdJsonPath_);
        json << "{\"file_format_version\":\"1.0.0\",\"ICD\":{\"library_path\":\""
             << LCL_VULKAN_ICD_PATH << "\",\"api_version\":\"1.1.0\"}}";
        json.close();
        ASSERT_EQ(setenv("VK_ICD_FILENAMES", icdJsonPath_.c_str(), 1), 0);
        server_ = std::thread([this] { serveOneHandshake(); });
    }

    void TearDown() override {
        if (server_.joinable()) server_.join();
        if (listener_ >= 0) close(listener_);
        unlink(socketPath_.c_str());
        unlink(icdJsonPath_.c_str());
        unsetenv("LCL_GPU_SOCKET");
        unsetenv("VK_ICD_FILENAMES");
    }

private:
    void serveOneHandshake() {
        const int client = accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) return;
        lcl::gpu_protocol::Header header{};
        std::vector<uint8_t> payload;
        if (lcl::gpu_protocol::receivePacket(client, header, payload) !=
            lcl::gpu_protocol::ReceiveStatus::Received) {
            close(client);
            return;
        }
        lcl::gpu_protocol::DeviceInfo device{};
        device.vulkanApiVersion = VK_API_VERSION_1_1;
        device.vendorId = 0x1c1;
        device.deviceId = 7;
        device.maxImageDimension2D = 4096;
        std::strncpy(device.deviceName, "LCL broker test GPU", sizeof(device.deviceName) - 1);
        (void)lcl::gpu_protocol::sendPacket(client, lcl::gpu_protocol::Opcode::DeviceInfo,
                                             &device, sizeof(device));
        close(client);
    }
};

TEST_F(LclVulkanIcdTest, NegotiatesLoaderAndDiscoversBrokeredDevice) {
    uint32_t loaderVersion = 7;
    ASSERT_EQ(vk_icdNegotiateLoaderICDInterfaceVersion(&loaderVersion), VK_SUCCESS);
    EXPECT_EQ(loaderVersion, 5u);
    ASSERT_NE(vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"), nullptr);

    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create.pApplicationInfo = &application;
    void* loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    ASSERT_NE(loader, nullptr) << dlerror();
    const auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(
        dlsym(loader, "vkCreateInstance"));
    const auto destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
        dlsym(loader, "vkDestroyInstance"));
    const auto enumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        dlsym(loader, "vkEnumeratePhysicalDevices"));
    const auto getPhysicalDeviceProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            dlsym(loader, "vkGetPhysicalDeviceProperties"));
    ASSERT_NE(createInstance, nullptr);
    ASSERT_NE(destroyInstance, nullptr);
    ASSERT_NE(enumeratePhysicalDevices, nullptr);
    ASSERT_NE(getPhysicalDeviceProperties, nullptr);

    VkInstance instance = VK_NULL_HANDLE;
    ASSERT_EQ(createInstance(&create, nullptr, &instance), VK_SUCCESS);

    uint32_t count = 0;
    ASSERT_EQ(enumeratePhysicalDevices(instance, &count, nullptr), VK_SUCCESS);
    ASSERT_EQ(count, 1u);
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    ASSERT_EQ(enumeratePhysicalDevices(instance, &count, &physical), VK_SUCCESS);
    VkPhysicalDeviceProperties properties{};
    getPhysicalDeviceProperties(physical, &properties);
    EXPECT_EQ(properties.vendorID, 0x1c1u);
    EXPECT_EQ(properties.limits.maxImageDimension2D, 4096u);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = 0;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceCreate{};
    deviceCreate.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreate.queueCreateInfoCount = 1;
    deviceCreate.pQueueCreateInfos = &queue;
    const auto createDevice = reinterpret_cast<PFN_vkCreateDevice>(
        dlsym(loader, "vkCreateDevice"));
    ASSERT_NE(createDevice, nullptr);
    VkDevice device = VK_NULL_HANDLE;
    ASSERT_EQ(createDevice(physical, &deviceCreate, nullptr, &device), VK_SUCCESS);
    const auto getDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(
        vkGetDeviceProcAddr(device, "vkGetDeviceQueue"));
    const auto destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(
        vkGetDeviceProcAddr(device, "vkDestroyDevice"));
    ASSERT_NE(getDeviceQueue, nullptr);
    ASSERT_NE(destroyDevice, nullptr);
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    getDeviceQueue(device, 0, 0, &graphicsQueue);
    EXPECT_NE(graphicsQueue, VK_NULL_HANDLE);
    destroyDevice(device, nullptr);
    destroyInstance(instance, nullptr);
    dlclose(loader);
}

} // namespace
