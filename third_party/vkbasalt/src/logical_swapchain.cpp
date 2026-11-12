#include "logical_swapchain.hpp"

namespace vkBasalt
{
    void LogicalSwapchain::destroy()
    {
        // Passthrough: nothing of ours is left (what a failed setup had made went when it failed)
        if (imageCount > 0 && !passthrough)
        {
            // The last effect submits may still read the effect resources.
            pLogicalDevice->vkd.QueueWaitIdle(pLogicalDevice->queue);
            effects.clear();

            // Also called on a setup that failed half way (vkBasalt_GetSwapchainImagesKHR): each list may be empty
            if (!commandBuffersEffect.empty())
                pLogicalDevice->vkd.FreeCommandBuffers(
                    pLogicalDevice->device, pLogicalDevice->commandPool, commandBuffersEffect.size(), commandBuffersEffect.data());
            Logger::debug("after free commandbuffer");

            pLogicalDevice->vkd.FreeMemory(pLogicalDevice->device, fakeImageMemory, nullptr);

            for (uint32_t i = 0; i < fakeImages.size(); i++)
            {
                pLogicalDevice->vkd.DestroyImage(pLogicalDevice->device, fakeImages[i], nullptr);
            }

            for (VkSemaphore semaphore : semaphores)
            {
                pLogicalDevice->vkd.DestroySemaphore(pLogicalDevice->device, semaphore, nullptr);
            }
            Logger::debug("after DestroySemaphore");
        }
    }
} // namespace vkBasalt
