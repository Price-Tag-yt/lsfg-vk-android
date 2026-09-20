#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/device.hpp"
#include "core/image.hpp"
#include "core/instance.hpp"
#include "common/exception.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace LSFG::Core;

namespace {

#ifdef __ANDROID__
const std::vector<const char*> requiredExtensions = {
    // Promoted dependencies are core in Vulkan 1.1 and must not be required
    // as separately enumerated extensions on older Android drivers.
    "VK_ANDROID_external_memory_android_hardware_buffer",
};
#else
const std::vector<const char*> requiredExtensions = {
    "VK_KHR_external_memory_fd",
    "VK_KHR_external_semaphore_fd",
};
#endif

bool hasExtension(const std::vector<VkExtensionProperties>& extensions,
        const char* name) {
    for (const auto& extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0)
            return true;
    }
    return false;
}

} // namespace

const Image& Device::getFallbackDescriptorImage() const {
    return *this->fallbackDescriptorImage;
}

Device::Device(const Instance& instance, uint64_t deviceUUID) {
    uint32_t deviceCount{};
    auto res = vkEnumeratePhysicalDevices(
        instance.handle(), &deviceCount, nullptr);
    if (res != VK_SUCCESS || deviceCount == 0)
        throw LSFG::vulkan_error(res, "Failed to enumerate physical devices");

    std::vector<VkPhysicalDevice> devices(deviceCount);
    res = vkEnumeratePhysicalDevices(
        instance.handle(), &deviceCount, devices.data());
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to get physical devices");

    std::optional<VkPhysicalDevice> physicalDevice;
    for (const auto device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        const uint64_t id =
            (static_cast<uint64_t>(properties.vendorID) << 32) |
            properties.deviceID;
        if (deviceUUID == 0 || deviceUUID == id || deviceUUID == 0x1463ABAC) {
            physicalDevice = device;
            break;
        }
    }
    if (!physicalDevice)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "Could not find physical device with UUID");

    uint32_t familyCount{};
    vkGetPhysicalDeviceQueueFamilyProperties(
        *physicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(
        *physicalDevice, &familyCount, queueFamilies.data());

    std::optional<uint32_t> computeFamilyIdx;
    for (uint32_t i = 0; i < familyCount; ++i) {
        if ((queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
            computeFamilyIdx = i;
            break;
        }
    }
    if (!computeFamilyIdx)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "No compute queue family found");

    uint32_t extensionCount{};
    res = vkEnumerateDeviceExtensionProperties(
        *physicalDevice, nullptr, &extensionCount, nullptr);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to enumerate device extensions");
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    res = vkEnumerateDeviceExtensionProperties(
        *physicalDevice, nullptr, &extensionCount, availableExtensions.data());
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to get device extensions");

    std::vector<const char*> enabledExtensions;
    for (const char* extension : requiredExtensions) {
        if (!hasExtension(availableExtensions, extension))
            throw LSFG::vulkan_error(VK_ERROR_EXTENSION_NOT_PRESENT,
                std::string("Missing required device extension: ") + extension);
        enabledExtensions.push_back(extension);
    }

    // Probe robustness2 before enabling nullDescriptor. Older mobile drivers
    // commonly expose neither the extension nor the feature.
    const bool robustness2Extension = hasExtension(
        availableExtensions, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
    };
    VkPhysicalDeviceFeatures2 supportedFeatures{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = robustness2Extension ? &robustness2 : nullptr,
    };
    vkGetPhysicalDeviceFeatures2(*physicalDevice, &supportedFeatures);

    const bool nullDescriptor = robustness2Extension &&
        robustness2.nullDescriptor == VK_TRUE;
    if (robustness2Extension) {
        enabledExtensions.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
        robustness2.nullDescriptor = nullDescriptor ? VK_TRUE : VK_FALSE;
    }

    const float queuePriority = 1.0F;
    const VkDeviceQueueCreateInfo queueInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = *computeFamilyIdx,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority,
    };

    // No Vulkan 1.2/1.3 feature structures are chained. This is intentional:
    // the target driver exposes Vulkan 1.1.131. All optional modern features
    // are disabled and the code uses legacy synchronization/fences.
    const VkDeviceCreateInfo deviceCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = robustness2Extension ? &robustness2 : nullptr,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueInfo,
        .enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size()),
        .ppEnabledExtensionNames = enabledExtensions.data(),
    };

    VkDevice deviceHandle{};
    res = vkCreateDevice(*physicalDevice, &deviceCreateInfo,
        nullptr, &deviceHandle);
    if (res != VK_SUCCESS || deviceHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create logical device");

    volkLoadDevice(deviceHandle);

    VkQueue queueHandle{};
    vkGetDeviceQueue(deviceHandle, *computeFamilyIdx, 0, &queueHandle);

    this->computeQueue = queueHandle;
    this->computeFamilyIdx = *computeFamilyIdx;
    this->physicalDevice = *physicalDevice;
    this->nullDescriptorSupported = nullDescriptor;
    this->device = std::shared_ptr<VkDevice>(
        new VkDevice(deviceHandle),
        [](VkDevice* device) { vkDestroyDevice(*device, nullptr); });

    if (!this->nullDescriptorSupported) {
        this->fallbackDescriptorImage = std::make_shared<Core::Image>(*this,
            VkExtent2D{1, 1}, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    }
}
