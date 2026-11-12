#ifndef EFFECT_LUT_HPP_INCLUDED
#define EFFECT_LUT_HPP_INCLUDED
#include "frame_gaze.hpp"
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <memory>
#include <array>
#include <chrono>

#include "vulkan_include.hpp"

#include "effect_simple.hpp"
#include "config.hpp"

namespace vkBasalt
{
    class LutEffect : public SimpleEffect
    {
    public:
        LutEffect(LogicalDevice*       pLogicalDevice,
                  VkFormat             format,
                  VkExtent2D           imageExtent,
                  std::vector<VkImage> inputImages,
                  std::vector<VkImage> outputImages,
                  Config*              pConfig);
        ~LutEffect();
        void applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer) override;
        void updateEffect() override;
        void prepareImage(uint32_t imageIndex) override;

    private:
        // FrameColor: every handle is null until made. A constructor that throws (VulkanFailure) frees what it made
        // with release(): ~LutEffect doesn't run then
        VkImage               lutImage               = VK_NULL_HANDLE;
        VkDeviceMemory        lutMemory              = VK_NULL_HANDLE;
        VkImageView           lutImageView           = VK_NULL_HANDLE;
        VkDescriptorSetLayout lutDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool      lutDescriptorPool      = VK_NULL_HANDLE;
        VkDescriptorSet       lutDescriptorSet       = VK_NULL_HANDLE;
        // One {sharpen amount, bypass, sharpen whole image} uniform buffer per swapchain image.
        std::vector<VkBuffer>        paramBuffers;
        std::vector<VkDeviceMemory>  paramMemory;
        std::vector<float*>          paramSlots;
        std::vector<bool>            paramSlotStale;
        std::vector<VkDescriptorSet> paramDescriptorSets;
        VkDescriptorSetLayout        paramDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool             paramDescriptorPool      = VK_NULL_HANDLE;
        // {sharpen amount, bypass, area (0 centre, 1 whole image, 2 follow gaze), gaze state (0 none, 1 valid,
        //  +2 debug ring), left eye u, v, right eye u, v}: the shader's FrameColorParams block, std140 floats
        static constexpr size_t kParamCount = 8;
        std::array<float, kParamCount> params{};
        // Gaze-following (frame_gaze.hpp): the shared record Frame Dock writes, mapped read-only on first use
        const void* gazeRecord = nullptr;
        std::chrono::steady_clock::time_point nextGazeOpen{};
        bool gazeDebug = false, gazeDot = false;
        // The debug part of the shader's gaze state: + 2 tint the sharpened area, + 4 also its centre dot
        float debugState() const { return gazeDebug ? (gazeDot ? 6.0f : 2.0f) : 0.0f; }
        framegaze::Sample lastGaze{};
        void updateGaze(std::chrono::steady_clock::time_point now);
        bool                         bypassArmed = false;
        uint32_t              lutSize = 0;
        bool                  hasFrameColorValues = false;
        std::array<float, 15> frameColorValues{};
        std::chrono::steady_clock::time_point nextFrameColorCheck{};
        // current.txt as last read: it is read again only when one of these changed (one stat() per check)
        bool     haveValuesStat = false;
        uint64_t valuesInode = 0, valuesSize = 0;
        int64_t  valuesMtimeNs = 0;
        // A new look goes up without QueueWaitIdle: the copy is ordered after the frames already submitted (its
        // barrier's first scope) and before the next ones on the same queue, so a slider drag never stalls the
        // compositor. One upload at a time: its fence says when the staging buffer and command buffer are free
        VkBuffer        uploadStaging       = VK_NULL_HANDLE;
        VkDeviceMemory  uploadStagingMemory = VK_NULL_HANDLE;
        unsigned char*  uploadMapped        = nullptr;
        VkFence         uploadFence         = VK_NULL_HANDLE;
        VkCommandBuffer uploadCommands      = VK_NULL_HANDLE;
        enum class Upload
        {
            Started,  ///< submitted: the next frames read the new LUT
            Busy,     ///< the previous upload is still running: try again at the next check
            Retry,    ///< Vulkan refused for now (allocation, submit): try again a little later
            Failed,   ///< no staging buffer or fence: trying again won't help
        };
        Upload uploadLut(const std::vector<unsigned char>& pixels);
        /** Frees what this part of the effect made (the handles that aren't null), on pLogicalDevice */
        void release(LogicalDevice* pLogicalDevice);
        /** Fills pixels (lutSize³ RGBA) with the look of current.txt's first 12 values */
        void buildLutPixels(const std::array<float, 15>& values, std::vector<unsigned char>& pixels) const;
        /** Tries to upload lookPixels (the look of pendingLook); keeps it pending while that may still work */
        void tryUploadLook(std::chrono::steady_clock::time_point now);
        // The look on screen (current.txt's first 12 values as last uploaded) and one waiting to go up. Sharpen and
        // bypass never wait for it
        bool                                  hasShownLook = false;
        std::array<float, 12>                 shownLook{};
        bool                                  lookPending = false;
        bool                                  lookRetryLogged = false;
        std::array<float, 15>                 pendingLook{};
        std::vector<unsigned char>            lookPixels;
        std::chrono::steady_clock::time_point lookRetryAt{};
    };
} // namespace vkBasalt

#endif // EFFECT_LUT_HPP_INCLUDED
