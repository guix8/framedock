#include "framebuffer.hpp"

namespace vkBasalt
{
    std::vector<VkFramebuffer>
    createFramebuffers(LogicalDevice* pLogicalDevice, VkRenderPass renderPass, VkExtent2D& extent, std::vector<std::vector<VkImageView>> imageViews)
    {
        std::vector<VkFramebuffer> framebuffers;
        framebuffers.reserve(imageViews[0].size());
        std::vector<VkImageView>   perFrameImageViews;
        // FrameColor: one that fails (VulkanFailure) frees the ones made before it
        try
        {
            for (uint32_t i = 0; i < imageViews[0].size(); i++)
            {
                for (auto& iv : imageViews)
                {
                    perFrameImageViews.push_back(iv[i]);
                }

                VkFramebufferCreateInfo framebufferCreateInfo;
                framebufferCreateInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                framebufferCreateInfo.pNext           = nullptr;
                framebufferCreateInfo.flags           = 0;
                framebufferCreateInfo.renderPass      = renderPass;
                framebufferCreateInfo.attachmentCount = perFrameImageViews.size();
                framebufferCreateInfo.pAttachments    = perFrameImageViews.data();
                framebufferCreateInfo.width           = extent.width;
                framebufferCreateInfo.height          = extent.height;
                framebufferCreateInfo.layers          = 1;

                VkFramebuffer framebuffer;
                VkResult      result = pLogicalDevice->vkd.CreateFramebuffer(pLogicalDevice->device, &framebufferCreateInfo, nullptr, &framebuffer);
                ASSERT_VULKAN(result);
                framebuffers.push_back(framebuffer);
                perFrameImageViews.clear();
            }
        }
        catch (...)
        {
            for (VkFramebuffer framebuffer : framebuffers)
                pLogicalDevice->vkd.DestroyFramebuffer(pLogicalDevice->device, framebuffer, nullptr);
            throw;
        }
        return framebuffers;
    }
} // namespace vkBasalt
