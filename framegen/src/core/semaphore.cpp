#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/semaphore.hpp"
#include "core/device.hpp"
#include "common/exception.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>

using namespace LSFG::Core;

Semaphore::Semaphore(const Core::Device& device, std::optional<uint32_t> initial) {
    // The Vulkan 1.1 baseline uses binary semaphores. A timeline semaphore
    // cannot be emulated by VkSemaphore; callers must use a Fence instead.
    if (initial.has_value())
        throw std::logic_error("Timeline semaphores are unavailable on this device");

    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    VkSemaphore semaphoreHandle{};
    const auto res = vkCreateSemaphore(device.handle(), &desc, nullptr,
        &semaphoreHandle);
    if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Unable to create semaphore");

    this->isTimeline = false;
    this->semaphore = std::shared_ptr<VkSemaphore>(
        new VkSemaphore(semaphoreHandle),
        [dev = device.handle()](VkSemaphore* semaphore) {
            vkDestroySemaphore(dev, *semaphore, nullptr);
        });
}

Semaphore::Semaphore(const Core::Device& device, int fd) {
    const VkExportSemaphoreCreateInfo exportInfo{
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &exportInfo,
    };
    VkSemaphore semaphoreHandle{};
    auto res = vkCreateSemaphore(device.handle(), &desc, nullptr,
        &semaphoreHandle);
    if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Unable to create semaphore");

    const auto importSemaphoreFd = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
        vkGetDeviceProcAddr(device.handle(), "vkImportSemaphoreFdKHR"));
    if (importSemaphoreFd == nullptr)
        throw LSFG::vulkan_error(VK_ERROR_EXTENSION_NOT_PRESENT,
            "vkImportSemaphoreFdKHR is unavailable");

    const VkImportSemaphoreFdInfoKHR importInfo{
        .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
        .semaphore = semaphoreHandle,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
        .fd = fd,
    };
    res = importSemaphoreFd(device.handle(), &importInfo);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Unable to import semaphore from fd");

    this->isTimeline = false;
    this->semaphore = std::shared_ptr<VkSemaphore>(
        new VkSemaphore(semaphoreHandle),
        [dev = device.handle()](VkSemaphore* semaphore) {
            vkDestroySemaphore(dev, *semaphore, nullptr);
        });
}

void Semaphore::signal(const Core::Device&, uint64_t) const {
    throw std::logic_error("Timeline semaphores are unavailable on this device");
}

bool Semaphore::wait(const Core::Device&, uint64_t, uint64_t) const {
    throw std::logic_error("Timeline semaphores are unavailable on this device");
}
