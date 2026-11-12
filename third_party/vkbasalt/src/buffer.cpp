#include "buffer.hpp"
#include "memory.hpp"

namespace vkBasalt
{
    void createBuffer(LogicalDevice*        pLogicalDevice,
                      VkDeviceSize          size,
                      VkBufferUsageFlags    usage,
                      VkMemoryPropertyFlags properties,
                      VkBuffer&             buffer,
                      VkDeviceMemory&       bufferMemory)
    {
        VkBufferCreateInfo bufferInfo = {};

        bufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size        = size;
        bufferInfo.usage       = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        // FrameColor: buffer and bufferMemory are set only when all went well. A step that fails (VulkanFailure)
        // frees what came before it
        VkBuffer newBuffer;
        VkResult result = pLogicalDevice->vkd.CreateBuffer(pLogicalDevice->device, &bufferInfo, nullptr, &newBuffer);
        ASSERT_VULKAN(result);

        VkDeviceMemory newMemory = VK_NULL_HANDLE;
        try
        {
            VkMemoryRequirements memRequirements;
            pLogicalDevice->vkd.GetBufferMemoryRequirements(pLogicalDevice->device, newBuffer, &memRequirements);

            VkMemoryAllocateInfo allocInfo = {};

            allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize  = memRequirements.size;
            allocInfo.memoryTypeIndex = findMemoryTypeIndex(pLogicalDevice, memRequirements.memoryTypeBits, properties);

            VkDeviceMemory allocated;
            result = pLogicalDevice->vkd.AllocateMemory(pLogicalDevice->device, &allocInfo, nullptr, &allocated);
            ASSERT_VULKAN(result);
            newMemory = allocated;

            result = pLogicalDevice->vkd.BindBufferMemory(pLogicalDevice->device, newBuffer, newMemory, 0);
            ASSERT_VULKAN(result);
        }
        catch (...)
        {
            if (newMemory)
                pLogicalDevice->vkd.FreeMemory(pLogicalDevice->device, newMemory, nullptr);
            pLogicalDevice->vkd.DestroyBuffer(pLogicalDevice->device, newBuffer, nullptr);
            throw;
        }
        buffer       = newBuffer;
        bufferMemory = newMemory;
    }

} // namespace vkBasalt
