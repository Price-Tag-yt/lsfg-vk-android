#include <volk.h>
#include <vulkan/vulkan_core.h>
#include "v3_1p/context.hpp"
#include "common/utils.hpp"
#include "common/exception.hpp"
#include <vector>
#include <cstddef>
#include <algorithm>
#include <optional>
#include <cstdint>

using namespace LSFG;
using namespace LSFG_3_1P;

#ifdef __ANDROID__
namespace {
void add_external_acquire(std::vector<VkImageMemoryBarrier>& barriers, Vulkan& vk, Core::Image& image, VkAccessFlags accessMask) {
    if (!image.isExternalShared()) return;
    barriers.emplace_back(VkImageMemoryBarrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = accessMask, .oldLayout = image.getLayout(), .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL, .dstQueueFamilyIndex = vk.device.getComputeFamilyIdx(), .image = image.handle(), .subresourceRange = {.aspectMask = image.getAspectFlags(), .levelCount = 1, .layerCount = 1}});
}
void add_external_release(std::vector<VkImageMemoryBarrier>& barriers, Vulkan& vk, Core::Image& image, VkAccessFlags accessMask) {
    if (!image.isExternalShared()) return;
    barriers.emplace_back(VkImageMemoryBarrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = accessMask, .dstAccessMask = 0, .oldLayout = image.getLayout(), .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = vk.device.getComputeFamilyIdx(), .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL, .image = image.handle(), .subresourceRange = {.aspectMask = image.getAspectFlags(), .levelCount = 1, .layerCount = 1}});
}
void emit_external_barriers(const Core::CommandBuffer& buf, const std::vector<VkImageMemoryBarrier>& barriers) {
    if (barriers.empty()) return;
    vkCmdPipelineBarrier(buf.handle(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, static_cast<uint32_t>(barriers.size()), barriers.data());
}
void emit_external_release_barriers(const Core::CommandBuffer& buf, const std::vector<VkImageMemoryBarrier>& barriers) {
    if (barriers.empty()) return;
    vkCmdPipelineBarrier(buf.handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, static_cast<uint32_t>(barriers.size()), barriers.data());
}
}
#endif

Context::Context(Vulkan& vk, int in0, int in1, const std::vector<int>& outN, VkExtent2D extent, VkFormat format) {
    this->inImg_0 = Core::Image(vk.device, extent, format, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, in0);
    this->inImg_1 = Core::Image(vk.device, extent, format, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, in1);
    for (size_t i = 0; i < 8; i++) { auto& data = this->data.at(i); data.internalSemaphores.resize(vk.generationCount); data.outSemaphores.resize(vk.generationCount); data.completionFences.resize(vk.generationCount); data.cmdBuffers2.resize(vk.generationCount); }
    this->mipmaps = Shaders::Mipmaps(vk, this->inImg_0, this->inImg_1);
    for (size_t i = 0; i < 7; i++) this->alpha.at(i) = Shaders::Alpha(vk, this->mipmaps.getOutImages().at(i));
    this->beta = Shaders::Beta(vk, this->alpha.at(0).getOutImages());
    for (size_t i = 0; i < 7; i++) { this->gamma.at(i) = Shaders::Gamma(vk, this->alpha.at(6-i).getOutImages(), this->beta.getOutImages().at(std::min<size_t>(6-i,5)), i == 0 ? std::nullopt : std::make_optional(this->gamma.at(i-1).getOutImage())); if (i < 4) continue; this->delta.at(i-4) = Shaders::Delta(vk, this->alpha.at(6-i).getOutImages(), this->beta.getOutImages().at(6-i), i == 4 ? std::nullopt : std::make_optional(this->gamma.at(i-1).getOutImage()), i == 4 ? std::nullopt : std::make_optional(this->delta.at(i-5).getOutImage1()), i == 4 ? std::nullopt : std::make_optional(this->delta.at(i-5).getOutImage2())); }
    this->generate = Shaders::Generate(vk, this->inImg_0, this->inImg_1, this->gamma.at(6).getOutImage(), this->delta.at(2).getOutImage1(), this->delta.at(2).getOutImage2(), outN, format);
}

void Context::present(Vulkan& vk, int inSem, const std::vector<int>& outSem) {
    auto& data = this->data.at(this->frameIdx % 8);
    if (data.shouldWait) for (auto& fence : data.completionFences) if (!fence.wait(vk.device, UINT64_MAX)) throw LSFG::vulkan_error(VK_TIMEOUT, "Fence wait timed out");
    data.shouldWait = true;
    if (inSem >= 0) data.inSemaphore = Core::Semaphore(vk.device, inSem);
    for (size_t i = 0; i < vk.generationCount; i++) data.internalSemaphores.at(i) = Core::Semaphore(vk.device);
    data.cmdBuffer1 = Core::CommandBuffer(vk.device, vk.commandPool); data.cmdBuffer1.begin();
#ifdef __ANDROID__
    { std::vector<VkImageMemoryBarrier> barriers; barriers.reserve(2); add_external_acquire(barriers, vk, this->inImg_0, VK_ACCESS_SHADER_READ_BIT); add_external_acquire(barriers, vk, this->inImg_1, VK_ACCESS_SHADER_READ_BIT); emit_external_barriers(data.cmdBuffer1, barriers); }
#endif
    this->mipmaps.Dispatch(data.cmdBuffer1, this->frameIdx); for (size_t i = 0; i < 7; i++) this->alpha.at(6-i).Dispatch(data.cmdBuffer1, this->frameIdx); this->beta.Dispatch(data.cmdBuffer1, this->frameIdx); data.cmdBuffer1.end();
    std::vector<Core::Semaphore> waits = {data.inSemaphore}; if (inSem < 0) waits.clear(); data.cmdBuffer1.submit(vk.device.getComputeQueue(), std::nullopt, waits, std::nullopt, data.internalSemaphores, std::nullopt);
    for (size_t pass = 0; pass < vk.generationCount; pass++) { auto& internalSemaphore = data.internalSemaphores.at(pass); auto& outSemaphore = data.outSemaphores.at(pass); if (inSem >= 0) outSemaphore = Core::Semaphore(vk.device, outSem.empty() ? -1 : outSem.at(pass)); auto& completionFence = data.completionFences.at(pass); completionFence = Core::Fence(vk.device); auto& buf = data.cmdBuffers2.at(pass); buf = Core::CommandBuffer(vk.device, vk.commandPool); buf.begin();
#ifdef __ANDROID__
        { std::vector<VkImageMemoryBarrier> barriers; add_external_acquire(barriers, vk, this->generate.getOutImages().at(pass), VK_ACCESS_SHADER_WRITE_BIT); emit_external_barriers(buf, barriers); }
#endif
        for (size_t i = 0; i < 7; i++) { this->gamma.at(i).Dispatch(buf, this->frameIdx, pass); if (i >= 4) this->delta.at(i-4).Dispatch(buf, this->frameIdx, pass); } this->generate.Dispatch(buf, this->frameIdx, pass);
#ifdef __ANDROID__
        { std::vector<VkImageMemoryBarrier> barriers; barriers.reserve(pass + 1 == vk.generationCount ? 3 : 1); add_external_release(barriers, vk, this->generate.getOutImages().at(pass), VK_ACCESS_SHADER_WRITE_BIT); if (pass + 1 == vk.generationCount) { add_external_release(barriers, vk, this->inImg_0, VK_ACCESS_SHADER_READ_BIT); add_external_release(barriers, vk, this->inImg_1, VK_ACCESS_SHADER_READ_BIT); } emit_external_release_barriers(buf, barriers); }
#endif
        buf.end(); std::vector<Core::Semaphore> signals = {outSemaphore}; if (inSem < 0) signals.clear(); buf.submit(vk.device.getComputeQueue(), completionFence, {internalSemaphore}, std::nullopt, signals, std::nullopt); }
    this->frameIdx++;
}

#ifdef __ANDROID__
#include <android/hardware_buffer.h>
Context::Context(Vulkan& vk, AHardwareBuffer* in0, AHardwareBuffer* in1, const std::vector<AHardwareBuffer*>& outN, VkExtent2D extent, VkFormat format) {
    this->inImg_0 = Core::Image(vk.device, extent, format, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, in0); this->inImg_1 = Core::Image(vk.device, extent, format, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, in1);
    std::vector<Core::Image> outImgs; outImgs.reserve(outN.size()); for (auto* ahb : outN) outImgs.emplace_back(vk.device, extent, format, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, ahb);
    for (size_t i = 0; i < 8; i++) { auto& data = this->data.at(i); data.internalSemaphores.resize(vk.generationCount); data.outSemaphores.resize(vk.generationCount); data.completionFences.resize(vk.generationCount); data.cmdBuffers2.resize(vk.generationCount); }
    this->mipmaps = Shaders::Mipmaps(vk, this->inImg_0, this->inImg_1); for (size_t i = 0; i < 7; i++) this->alpha.at(i) = Shaders::Alpha(vk, this->mipmaps.getOutImages().at(i)); this->beta = Shaders::Beta(vk, this->alpha.at(0).getOutImages());
    for (size_t i = 0; i < 7; i++) { this->gamma.at(i) = Shaders::Gamma(vk, this->alpha.at(6-i).getOutImages(), this->beta.getOutImages().at(std::min<size_t>(6-i,5)), i == 0 ? std::nullopt : std::make_optional(this->gamma.at(i-1).getOutImage())); if (i < 4) continue; this->delta.at(i-4) = Shaders::Delta(vk, this->alpha.at(6-i).getOutImages(), this->beta.getOutImages().at(6-i), i == 4 ? std::nullopt : std::make_optional(this->gamma.at(i-1).getOutImage()), i == 4 ? std::nullopt : std::make_optional(this->delta.at(i-5).getOutImage1()), i == 4 ? std::nullopt : std::make_optional(this->delta.at(i-5).getOutImage2())); }
    this->generate = Shaders::Generate(vk, this->inImg_0, this->inImg_1, this->gamma.at(6).getOutImage(), this->delta.at(2).getOutImage1(), this->delta.at(2).getOutImage2(), std::move(outImgs));
}
#endif
