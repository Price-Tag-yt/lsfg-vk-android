#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/commandbuffer.hpp"
#include "core/device.hpp"
#include "core/commandpool.hpp"
#include "core/fence.hpp"
#include "core/semaphore.hpp"
#include "common/exception.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace LSFG::Core;

CommandBuffer::CommandBuffer(const Core::Device& device, const CommandPool& pool) {
    const VkCommandBufferAllocateInfo desc{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool.handle(),
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer commandBufferHandle{};
    auto res = vkAllocateCommandBuffers(device.handle(), &desc, &commandBufferHandle);
    if (res != VK_SUCCESS || commandBufferHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Unable to allocate command buffer");

    this->state = std::make_shared<CommandBufferState>(CommandBufferState::Empty);
    this->commandBuffer = std::shared_ptr<VkCommandBuffer>(
        new VkCommandBuffer(commandBufferHandle),
        [dev = device.handle(), pool = pool.handle()](VkCommandBuffer* commandBuffer) {
            vkFreeCommandBuffers(dev, pool, 1, commandBuffer);
        });
}

void CommandBuffer::begin() {
    if (*this->state != CommandBufferState::Empty)
        throw std::logic_error("Command buffer is not in Empty state");
    const VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    const auto res = vkBeginCommandBuffer(*this->commandBuffer, &beginInfo);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Unable to begin command buffer");
    *this->state = CommandBufferState::Recording;
}

void CommandBuffer::dispatch(uint32_t x, uint32_t y, uint32_t z) const {
    if (*this->state != CommandBufferState::Recording)
        throw std::logic_error("Command buffer is not in Recording state");
    vkCmdDispatch(*this->commandBuffer, x, y, z);
}

void CommandBuffer::end() {
    if (*this->state != CommandBufferState::Recording)
        throw std::logic_error("Command buffer is not in Recording state");
    const auto res = vkEndCommandBuffer(*this->commandBuffer);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Unable to end command buffer");
    *this->state = CommandBufferState::Full;
}

void CommandBuffer::submit(VkQueue queue, std::optional<Fence> fence,
        const std::vector<Semaphore>& waitSemaphores,
        std::optional<std::vector<uint64_t>> waitSemaphoreValues,
        const std::vector<Semaphore>& signalSemaphores,
        std::optional<std::vector<uint64_t>> signalSemaphoreValues) {
    if (*this->state != CommandBufferState::Full)
        throw std::logic_error("Command buffer is not in Full state");

    // Vulkan 1.1 fallback: queue submission always uses binary semaphores.
    // Timeline values are intentionally rejected because this Device does not
    // enable VK_KHR_timeline_semaphore on drivers that lack it.
    if (waitSemaphoreValues.has_value() || signalSemaphoreValues.has_value())
        throw std::logic_error("Timeline semaphore submission is unavailable");

    const std::vector<VkPipelineStageFlags> waitStages(
        waitSemaphores.size(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    std::vector<VkSemaphore> waitHandles;
    waitHandles.reserve(waitSemaphores.size());
    for (const auto& semaphore : waitSemaphores)
        waitHandles.push_back(semaphore.handle());
    std::vector<VkSemaphore> signalHandles;
    signalHandles.reserve(signalSemaphores.size());
    for (const auto& semaphore : signalSemaphores)
        signalHandles.push_back(semaphore.handle());

    const VkSubmitInfo submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = static_cast<uint32_t>(waitHandles.size()),
        .pWaitSemaphores = waitHandles.data(),
        .pWaitDstStageMask = waitStages.data(),
        .commandBufferCount = 1,
        .pCommandBuffers = &(*this->commandBuffer),
        .signalSemaphoreCount = static_cast<uint32_t>(signalHandles.size()),
        .pSignalSemaphores = signalHandles.data(),
    };
    const auto res = vkQueueSubmit(queue, 1, &submitInfo,
        fence ? fence->handle() : VK_NULL_HANDLE);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Unable to submit command buffer");
    *this->state = CommandBufferState::Submitted;
}
