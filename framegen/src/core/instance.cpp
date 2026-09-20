#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/instance.hpp"
#include "common/exception.hpp"

#include <cstdint>
#include <memory>

using namespace LSFG::Core;

Instance::Instance() {
    const auto res = volkInitialize();
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to initialize Vulkan loader");

    // Vulkan 1.1 is the baseline for Helio P22 / PowerVR Rogue GE8320.
    // Do not request Vulkan 1.3 here: vkCreateInstance can fail on drivers
    // which expose only Vulkan 1.1.x.
    const VkApplicationInfo appInfo{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "lsfg-vk-base",
        .applicationVersion = VK_MAKE_VERSION(0, 0, 1),
        .pEngineName = "lsfg-vk-base",
        .engineVersion = VK_MAKE_VERSION(0, 0, 1),
        .apiVersion = VK_API_VERSION_1_1,
    };

    const VkInstanceCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
    };

    VkInstance instanceHandle{};
    const auto createResult = vkCreateInstance(
        &createInfo, nullptr, &instanceHandle);
    if (createResult != VK_SUCCESS || instanceHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(createResult, "Failed to create Vulkan instance");

    volkLoadInstance(instanceHandle);
    this->instance = std::shared_ptr<VkInstance>(
        new VkInstance(instanceHandle),
        [](VkInstance* instance) { vkDestroyInstance(*instance, nullptr); });
}
