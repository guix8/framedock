#include "image_view.hpp"

namespace vkBasalt
{
    std::vector<VkImageView> createImageViews(LogicalDevice*       pLogicalDevice,
                                              VkFormat             format,
                                              std::vector<VkImage> images,
                                              VkImageViewType      viewType,
                                              VkImageAspectFlags   aspectMask,
                                              uint32_t             mipLevels)
    {
        std::vector<VkImageView> imageViews;
        imageViews.reserve(images.size());

        VkImageViewCreateInfo imageViewCreateInfo;

        imageViewCreateInfo.sType        = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        imageViewCreateInfo.pNext        = nullptr;
        imageViewCreateInfo.flags        = 0;
        imageViewCreateInfo.image        = VK_NULL_HANDLE;
        imageViewCreateInfo.viewType     = viewType;
        imageViewCreateInfo.format       = format;
        imageViewCreateInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        imageViewCreateInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        imageViewCreateInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        imageViewCreateInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

        imageViewCreateInfo.subresourceRange.aspectMask     = aspectMask;
        imageViewCreateInfo.subresourceRange.baseMipLevel   = 0;
        imageViewCreateInfo.subresourceRange.levelCount     = mipLevels;
        imageViewCreateInfo.subresourceRange.baseArrayLayer = 0;
        imageViewCreateInfo.subresourceRange.layerCount     = 1;

        // FrameColor: a view that fails (VulkanFailure) frees the ones made before it
        try
        {
            for (uint32_t i = 0; i < images.size(); i++)
            {
                imageViewCreateInfo.image = images[i];
                VkImageView imageView;
                VkResult    result = pLogicalDevice->vkd.CreateImageView(pLogicalDevice->device, &imageViewCreateInfo, nullptr, &imageView);
                ASSERT_VULKAN(result);
                imageViews.push_back(imageView);
            }
        }
        catch (...)
        {
            for (VkImageView imageView : imageViews)
                pLogicalDevice->vkd.DestroyImageView(pLogicalDevice->device, imageView, nullptr);
            throw;
        }

        return imageViews;
    }

} // namespace vkBasalt
