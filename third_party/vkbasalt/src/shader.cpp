#include "shader.hpp"

#include <array>
#include <filesystem>

namespace vkBasalt
{
    void createShaderModule(LogicalDevice* pLogicalDevice, const std::vector<char>& code, VkShaderModule* shaderModule)
    {
        VkShaderModuleCreateInfo shaderCreateInfo;

        shaderCreateInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shaderCreateInfo.pNext    = nullptr;
        shaderCreateInfo.flags    = 0;
        shaderCreateInfo.codeSize = code.size();
        shaderCreateInfo.pCode    = (uint32_t*) code.data();

        // FrameColor: *shaderModule is set only when it was made (the destructor of a failed effect frees it)
        VkShaderModule newModule;
        VkResult       result = pLogicalDevice->vkd.CreateShaderModule(pLogicalDevice->device, &shaderCreateInfo, nullptr, &newModule);
        ASSERT_VULKAN(result);
        *shaderModule = newModule;
    }

    void createShaderModule(LogicalDevice* pLogicalDevice, const std::vector<uint32_t>& code, VkShaderModule* shaderModule)
    {
        VkShaderModuleCreateInfo shaderCreateInfo;

        shaderCreateInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shaderCreateInfo.pNext    = nullptr;
        shaderCreateInfo.flags    = 0;
        shaderCreateInfo.codeSize = code.size() * sizeof(uint32_t);
        shaderCreateInfo.pCode    = code.data();

        // FrameColor: *shaderModule is set only when it was made (the destructor of a failed effect frees it)
        VkShaderModule newModule;
        VkResult       result = pLogicalDevice->vkd.CreateShaderModule(pLogicalDevice->device, &shaderCreateInfo, nullptr, &newModule);
        ASSERT_VULKAN(result);
        *shaderModule = newModule;
    }
} // namespace vkBasalt
