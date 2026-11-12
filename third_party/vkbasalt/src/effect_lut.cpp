#include "effect_lut.hpp"

#include <cstring>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <locale>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/mman.h>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>

#include "image_view.hpp"
#include "descriptor_set.hpp"
#include "buffer.hpp"
#include "renderpass.hpp"
#include "graphics_pipeline.hpp"
#include "framebuffer.hpp"
#include "shader.hpp"
#include "sampler.hpp"
#include "image.hpp"
#include "lut_cube.hpp"
#include "frame_gaze.hpp"

#include "shader_sources.hpp"

namespace vkBasalt
{
    LutEffect::LutEffect(LogicalDevice*       pLogicalDevice,
                         VkFormat             format,
                         VkExtent2D           imageExtent,
                         std::vector<VkImage> inputImages,
                         std::vector<VkImage> outputImages,
                         Config*              pConfig)
    {
        // FrameColor: a failed step throws VulkanFailure (vkBasalt_GetSwapchainImagesKHR then leaves the swapchain
        // without the effect). ~LutEffect doesn't run for a constructor that throws, so what was made is freed here
        try
        {
            vertexCode   = full_screen_triangle_vert;
            fragmentCode = lut_frag;

            std::string lutFile = pConfig->lutFile();

            // FrameColor generates only .cube LUTs; PNG/image loaders are omitted.
            LutCube lutCube(lutFile);
            int32_t height = lutCube.size;
            const unsigned char* pixels = lutCube.colorCube.data();
            VkSpecializationMapEntry specMapEntry{0, 0, sizeof(int32_t)};

            VkSpecializationInfo fragmentSpecializationInfo;
            fragmentSpecializationInfo.mapEntryCount = 1;
            fragmentSpecializationInfo.pMapEntries   = &specMapEntry;
            fragmentSpecializationInfo.dataSize      = sizeof(height);
            fragmentSpecializationInfo.pData         = &height;

            pVertexSpecInfo   = nullptr;
            pFragmentSpecInfo = &fragmentSpecializationInfo;

            VkExtent3D lutImageExtent = {(uint32_t) height, (uint32_t) height, (uint32_t) height};
            lutSize = static_cast<uint32_t>(height);

            lutImage = createImages(pLogicalDevice,
                                    1,
                                    lutImageExtent,
                                    VK_FORMAT_R8G8B8A8_UNORM, // TODO search for format and save it
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                    lutMemory)[0];

            uploadToImage(pLogicalDevice, lutImage, lutImageExtent, height * height * height * 4, pixels);

            lutImageView = createImageViews(pLogicalDevice, VK_FORMAT_R8G8B8A8_UNORM, std::vector<VkImage>(1, lutImage), VK_IMAGE_VIEW_TYPE_3D)[0];

            // The live look's uploads (updateEffect): a staging buffer kept mapped and a fence, made once
            if (lutSize >= 2 && lutSize <= 65)
            {
                // Without them the look set at start stays on screen: no reason to drop the whole effect
                try
                {
                    createBuffer(pLogicalDevice,
                                 lutSize * lutSize * lutSize * 4,
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 uploadStaging,
                                 uploadStagingMemory);
                    VkResult mapped = pLogicalDevice->vkd.MapMemory(pLogicalDevice->device, uploadStagingMemory, 0, VK_WHOLE_SIZE, 0,
                                                                    reinterpret_cast<void**>(&uploadMapped));
                    if (mapped != VK_SUCCESS)
                        uploadMapped = nullptr;
                }
                catch (const VulkanFailure&)
                {
                    uploadStaging       = VK_NULL_HANDLE;
                    uploadStagingMemory = VK_NULL_HANDLE;
                    uploadMapped        = nullptr;
                }
                VkFenceCreateInfo fenceInfo{};
                fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
                if (pLogicalDevice->vkd.CreateFence(pLogicalDevice->device, &fenceInfo, nullptr, &uploadFence) != VK_SUCCESS)
                    uploadFence = VK_NULL_HANDLE;
                if (!uploadMapped || !uploadFence)
                    Logger::err("FrameColor: no live LUT updates (staging buffer or fence failed)");
            }

            lutDescriptorSetLayout = createImageSamplerDescriptorSetLayout(pLogicalDevice, 1);
            descriptorSetLayouts.push_back(lutDescriptorSetLayout);

            VkDescriptorPoolSize imagePoolSize;
            imagePoolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            imagePoolSize.descriptorCount = 1;

            std::vector<VkDescriptorPoolSize> poolSizes = {imagePoolSize};

            lutDescriptorPool = createDescriptorPool(pLogicalDevice, poolSizes);

            // The sharpen amount, the compare bypass and the sharpen area live in mapped uniform
            // buffers, so changes do not need new pipelines or command buffers.
            // Each swapchain image has its own buffer: prepareImage() rewrites a
            // buffer only when its image is presented again, so no queue wait is needed.
            const size_t imageCount = inputImages.size();
            paramBuffers.resize(imageCount);
            paramMemory.resize(imageCount);
            paramSlots.resize(imageCount);
            paramSlotStale.assign(imageCount, false);
            for (size_t i = 0; i < imageCount; ++i)
            {
                createBuffer(pLogicalDevice,
                             sizeof(float) * kParamCount,
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             paramBuffers[i],
                             paramMemory[i]);
                float*   slot   = nullptr;
                VkResult result = pLogicalDevice->vkd.MapMemory(
                    pLogicalDevice->device, paramMemory[i], 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&slot));
                ASSERT_VULKAN(result);
                paramSlots[i] = slot;
                std::fill(slot, slot + kParamCount, 0.0f);
                slot[4] = slot[5] = slot[6] = slot[7] = 0.5f;  // gaze points start at the lens centre
            }

            paramDescriptorSetLayout = createUniformBufferDescriptorSetLayout(pLogicalDevice);
            descriptorSetLayouts.push_back(paramDescriptorSetLayout);

            VkDescriptorPoolSize bufferPoolSize;
            bufferPoolSize.type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            bufferPoolSize.descriptorCount = static_cast<uint32_t>(imageCount);

            paramDescriptorPool = createDescriptorPool(pLogicalDevice, {bufferPoolSize});

            init(pLogicalDevice, format, imageExtent, inputImages, outputImages, pConfig);

            lutDescriptorSet =
                allocateAndWriteImageSamplerDescriptorSets(pLogicalDevice,
                                                           lutDescriptorPool,
                                                           lutDescriptorSetLayout,
                                                           {sampler},
                                                           std::vector<std::vector<VkImageView>>(1, std::vector<VkImageView>(1, lutImageView)))[0];

            for (VkBuffer buffer : paramBuffers)
                paramDescriptorSets.push_back(
                    writeBufferDescriptorSet(pLogicalDevice, paramDescriptorPool, paramDescriptorSetLayout, buffer));
        }
        catch (...)
        {
            release(pLogicalDevice);
            throw;
        }
    }
    LutEffect::~LutEffect()
    {
        release(pLogicalDevice);
    }
    void LutEffect::release(LogicalDevice* pLogicalDevice)
    {
        // The last upload may still be copying: it finishes (bounded wait) before its buffers go
        if (uploadFence)
        {
            pLogicalDevice->vkd.WaitForFences(pLogicalDevice->device, 1, &uploadFence, VK_TRUE, 1000000000ull);
            pLogicalDevice->vkd.DestroyFence(pLogicalDevice->device, uploadFence, nullptr);
        }
        if (uploadCommands)
            pLogicalDevice->vkd.FreeCommandBuffers(pLogicalDevice->device, pLogicalDevice->commandPool, 1, &uploadCommands);
        if (uploadMapped)
            pLogicalDevice->vkd.UnmapMemory(pLogicalDevice->device, uploadStagingMemory);
        if (uploadStaging)
            pLogicalDevice->vkd.DestroyBuffer(pLogicalDevice->device, uploadStaging, nullptr);
        if (uploadStagingMemory)
            pLogicalDevice->vkd.FreeMemory(pLogicalDevice->device, uploadStagingMemory, nullptr);
        if (lutImageView)
            pLogicalDevice->vkd.DestroyImageView(pLogicalDevice->device, lutImageView, nullptr);
        if (lutImage)
            pLogicalDevice->vkd.DestroyImage(pLogicalDevice->device, lutImage, nullptr);
        if (lutDescriptorSetLayout)
            pLogicalDevice->vkd.DestroyDescriptorSetLayout(pLogicalDevice->device, lutDescriptorSetLayout, nullptr);
        if (lutDescriptorPool)
            pLogicalDevice->vkd.DestroyDescriptorPool(pLogicalDevice->device, lutDescriptorPool, nullptr);
        if (lutMemory)
            pLogicalDevice->vkd.FreeMemory(pLogicalDevice->device, lutMemory, nullptr);
        for (size_t i = 0; i < paramBuffers.size(); ++i)
        {
            if (paramSlots[i])
                pLogicalDevice->vkd.UnmapMemory(pLogicalDevice->device, paramMemory[i]);
            if (paramBuffers[i])
                pLogicalDevice->vkd.DestroyBuffer(pLogicalDevice->device, paramBuffers[i], nullptr);
            if (paramMemory[i])
                pLogicalDevice->vkd.FreeMemory(pLogicalDevice->device, paramMemory[i], nullptr);
        }
        if (paramDescriptorSetLayout)
            pLogicalDevice->vkd.DestroyDescriptorSetLayout(pLogicalDevice->device, paramDescriptorSetLayout, nullptr);
        if (paramDescriptorPool)
            pLogicalDevice->vkd.DestroyDescriptorPool(pLogicalDevice->device, paramDescriptorPool, nullptr);
    }
    void LutEffect::applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer)
    {
        const std::array<VkDescriptorSet, 2> descriptorSets{{lutDescriptorSet, paramDescriptorSets[imageIndex]}};
        pLogicalDevice->vkd.CmdBindDescriptorSets(commandBuffer,
                                                  VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                  pipelineLayout,
                                                  1,
                                                  static_cast<uint32_t>(descriptorSets.size()),
                                                  descriptorSets.data(),
                                                  0,
                                                  nullptr);
        SimpleEffect::applyEffect(imageIndex, commandBuffer);
    }
    void LutEffect::updateEffect()
    {
        // The dashboard writes current.txt during slider drags and on release.
        const auto now = std::chrono::steady_clock::now();
        // The gaze changes every frame: read on every present, before the 50 ms file check below
        if (hasFrameColorValues && params[2] > 1.5f)
            updateGaze(now);
        if (now < nextFrameColorCheck)
            return;
        nextFrameColorCheck = now + std::chrono::milliseconds(50);
        const char* home = std::getenv("HOME");
        if (!home || lutSize < 2 || lutSize > 65)
            return;
        const char* xdgConfig = std::getenv("XDG_CONFIG_HOME");
        const std::string configRoot = xdgConfig && *xdgConfig ? xdgConfig : std::string(home) + "/.config";
        const std::string valuesPath = configRoot + "/framecolor/current.txt";
        // The gaze debug view: a file next to current.txt tints the sharpened area; a second one, gaze-debug-dot,
        // also marks its centre with a red dot (off unless that file is there: a dot invites looking at the dot
        // instead of the fit's target, 2026-11-11). Both checked every 50 ms, not per frame
        struct stat debugStat{};
        const bool debugNow = ::stat((configRoot + "/framecolor/gaze-debug").c_str(), &debugStat) == 0;
        const bool dotNow = ::stat((configRoot + "/framecolor/gaze-debug-dot").c_str(), &debugStat) == 0;
        if (debugNow != gazeDebug || dotNow != gazeDot)
        {
            gazeDebug = debugNow;
            gazeDot = dotNow;
            params[3] = std::fmod(params[3], 2.0f) + debugState();
            std::fill(paramSlotStale.begin(), paramSlotStale.end(), true);
        }
        // Unchanged since the last read (same file, size and time): nothing to read (one stat() instead of a parse)
        struct stat st{};
        if (::stat(valuesPath.c_str(), &st) != 0)
            return;
        const int64_t mtimeNs = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
        if (haveValuesStat && valuesInode == static_cast<uint64_t>(st.st_ino)
            && valuesSize == static_cast<uint64_t>(st.st_size) && valuesMtimeNs == mtimeNs)
        {
            // Nothing new in the file. A look whose upload couldn't start (the last copy still running) goes now
            if (lookPending && now >= lookRetryAt)
                tryUploadLook(now);
            return;
        }
        // Whatever the file turns out to hold, it was read: it is read again only when it changes (a file that
        // doesn't parse, or says nothing new, would otherwise be parsed every 50 ms)
        haveValuesStat = true;
        valuesInode    = static_cast<uint64_t>(st.st_ino);
        valuesSize     = static_cast<uint64_t>(st.st_size);
        valuesMtimeNs  = mtimeNs;

        std::ifstream valuesFile(valuesPath);
        valuesFile.imbue(std::locale::classic());  // "1.5", whatever the compositor's locale
        std::array<float, 15> values{{0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f,
                                      1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f}};
        std::vector<float> stored;
        for (float value; valuesFile >> value;)
            stored.push_back(value);
        if (stored.size() < 6 || stored.size() == 11)
            return;
        for (size_t i = 0; i < std::min(stored.size(), values.size()); ++i)
            values[i] = stored[i];
        // Older files have no gain, saturation, sharpen, bypass or sharpen area fields. The
        // ten-value file used one saturation setting for all three output channels.
        if (stored.size() == 10)
            values[10] = values[11] = values[9];
        if (!std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); }))
            return;
        // A dashboard that stopped while compare was held leaves bypass at 1.
        // Honour bypass only after this layer has seen it at 0 once, so that
        // stale flag cannot hide the correction.
        if (values[13] <= 0.5f)
            bypassArmed = true;
        const float bypass = bypassArmed && values[13] > 0.5f ? 1.0f : 0.0f;
        // The look on screen is the last one that went up (shownLook), not the last one read: one that couldn't go
        // up is tried again
        const bool lookChanged =
            !hasShownLook || !std::equal(values.begin(), values.begin() + 12, shownLook.begin());
        const bool paramsChanged = !hasFrameColorValues || values[12] != frameColorValues[12]
                                   || values[14] != frameColorValues[14] || bypass != params[1];

        // Sharpen and bypass first, and whatever happens to the look: a held compare must show the original image
        // even while a new look can't go up
        if (paramsChanged)
        {
            const float area = values[14] > 1.5f && values[14] < 2.5f ? 2.0f : values[14] > 0.5f ? 1.0f : 0.0f;
            params[0] = std::clamp(values[12], 0.0f, 3.0f);
            params[1] = bypass;
            params[2] = area;
            if (area < 1.5f)
                params[3] = debugState();  // no gaze point outside the gaze area
            std::fill(paramSlotStale.begin(), paramSlotStale.end(), true);
            Logger::debug("FrameColor sharpen " + std::to_string(params[0]) + " bypass " + std::to_string(params[1])
                          + " whole image " + std::to_string(params[2]));
        }
        frameColorValues    = values;
        hasFrameColorValues = true;
        if (lookChanged)
        {
            if (!lookPending || !std::equal(values.begin(), values.begin() + 12, pendingLook.begin()))
            {
                buildLutPixels(values, lookPixels);
                pendingLook     = values;
                lookRetryAt     = now;
                lookRetryLogged = false;
            }
            lookPending = true;
            if (now >= lookRetryAt)
                tryUploadLook(now);
        }
        else if (lookPending)
        {
            // Back to the look on screen before the new one could go up: nothing to upload
            lookPending = false;
            std::vector<unsigned char>().swap(lookPixels);
        }
    }
    void LutEffect::buildLutPixels(const std::array<float, 15>& values, std::vector<unsigned char>& pixels) const
    {
        std::array<std::array<unsigned char, 65>, 3> curves{};
        for (uint32_t channel = 0; channel < 3; ++channel)
        {
            float contrast = std::clamp(values[channel], 0.0f, 1.0f);
            float gamma = std::clamp(values[channel + 3], 0.50f, 2.50f);
            float gain = std::clamp(values[channel + 6], 0.50f, 1.50f);
            for (uint32_t point = 0; point < lutSize; ++point)
            {
                float x = static_cast<float>(point) / static_cast<float>(lutSize - 1);
                float color = std::pow(std::clamp(x * gain, 0.0f, 1.0f), 1.0f / gamma);
                float smooth = color * color * (3.0f - 2.0f * color);
                float corrected = std::clamp(color * (1.0f - contrast) + smooth * contrast, 0.0f, 1.0f);
                curves[channel][point] = static_cast<unsigned char>(std::lround(corrected * 255.0f));
            }
        }
        pixels.assign(static_cast<size_t>(lutSize) * lutSize * lutSize * 4, 255);
        const std::array<float, 3> saturation{{std::clamp(values[9], 0.0f, 2.0f),
                                                std::clamp(values[10], 0.0f, 2.0f),
                                                std::clamp(values[11], 0.0f, 2.0f)}};
        for (uint32_t blue = 0; blue < lutSize; ++blue)
            for (uint32_t green = 0; green < lutSize; ++green)
                for (uint32_t red = 0; red < lutSize; ++red)
                {
                    size_t offset = ((blue * lutSize + green) * lutSize + red) * 4;
                    if (saturation == std::array<float, 3>{{1.0f, 1.0f, 1.0f}})
                    {
                        pixels[offset] = curves[0][red];
                        pixels[offset + 1] = curves[1][green];
                        pixels[offset + 2] = curves[2][blue];
                        continue;
                    }
                    const float r = curves[0][red] / 255.0f;
                    const float g = curves[1][green] / 255.0f;
                    const float b = curves[2][blue] / 255.0f;
                    const float luminance = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    pixels[offset] = static_cast<unsigned char>(std::lround(
                        std::clamp(luminance + (r - luminance) * saturation[0], 0.0f, 1.0f) * 255.0f));
                    pixels[offset + 1] = static_cast<unsigned char>(std::lround(
                        std::clamp(luminance + (g - luminance) * saturation[1], 0.0f, 1.0f) * 255.0f));
                    pixels[offset + 2] = static_cast<unsigned char>(std::lround(
                        std::clamp(luminance + (b - luminance) * saturation[2], 0.0f, 1.0f) * 255.0f));
                }
    }
    void LutEffect::tryUploadLook(std::chrono::steady_clock::time_point now)
    {
        // Ordered on the queue (no QueueWaitIdle): see uploadLut
        const Upload result = uploadLut(lookPixels);
        switch (result)
        {
            case Upload::Started:
                std::copy(pendingLook.begin(), pendingLook.begin() + 12, shownLook.begin());
                hasShownLook = true;
                Logger::info("FrameColor live LUT updated");
                break;
            case Upload::Busy:
                return;  // the last copy is still running: the next check (50 ms) tries again, without reading
            case Upload::Retry:
                // Vulkan refused for now (out of memory, a failed submit): again in a second, logged once per look
                if (!lookRetryLogged)
                    Logger::err("FrameColor live LUT upload failed: trying again every second");
                lookRetryLogged = true;
                lookRetryAt     = now + std::chrono::seconds(1);
                return;
            case Upload::Failed:
                // No staging buffer or fence: trying again won't help. The look stays as it was (sharpen and
                // bypass still follow the file); the next change of current.txt tries again
                Logger::err("FrameColor live LUT upload isn't possible: the look stays as it was");
                break;
        }
        lookPending     = false;
        lookRetryLogged = false;
        std::vector<unsigned char>().swap(lookPixels);
    }
    LutEffect::Upload LutEffect::uploadLut(const std::vector<unsigned char>& pixels)
    {
        if (!uploadMapped || !uploadFence || pixels.size() != static_cast<size_t>(lutSize) * lutSize * lutSize * 4)
            return Upload::Failed;
        const VkDevice device = pLogicalDevice->device;
        if (uploadCommands)
        {
            if (pLogicalDevice->vkd.GetFenceStatus(device, uploadFence) != VK_SUCCESS)
                return Upload::Busy;  // still copying the last one (or the device is gone)
            pLogicalDevice->vkd.FreeCommandBuffers(device, pLogicalDevice->commandPool, 1, &uploadCommands);
            uploadCommands = VK_NULL_HANDLE;
        }
        std::memcpy(uploadMapped, pixels.data(), pixels.size());

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool        = pLogicalDevice->commandPool;
        allocInfo.commandBufferCount = 1;
        if (pLogicalDevice->vkd.AllocateCommandBuffers(device, &allocInfo, &uploadCommands) != VK_SUCCESS)
        {
            uploadCommands = VK_NULL_HANDLE;
            return Upload::Retry;
        }
        initializeDispatchTable(uploadCommands, device);
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        pLogicalDevice->vkd.BeginCommandBuffer(uploadCommands, &beginInfo);

        // Its first scope is everything earlier in submission order on this queue: the frames already submitted
        // finish reading the LUT before the copy writes it
        VkImageMemoryBarrier barrier{};
        barrier.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask               = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout                   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        barrier.image                       = lutImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        pLogicalDevice->vkd.CmdPipelineBarrier(uploadCommands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent                 = {lutSize, lutSize, lutSize};
        pLogicalDevice->vkd.CmdCopyBufferToImage(uploadCommands, uploadStaging, lutImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                                 &region);
        // And its second scope everything later: the next frames read the new LUT
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        pLogicalDevice->vkd.CmdPipelineBarrier(uploadCommands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                               0, 0, nullptr, 0, nullptr, 1, &barrier);
        pLogicalDevice->vkd.EndCommandBuffer(uploadCommands);

        VkSubmitInfo submitInfo{};
        submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers    = &uploadCommands;
        pLogicalDevice->vkd.ResetFences(device, 1, &uploadFence);
        if (pLogicalDevice->vkd.QueueSubmit(pLogicalDevice->queue, 1, &submitInfo, uploadFence) != VK_SUCCESS)
        {
            // Not submitted: the fence would never signal. Signal it again by recreating it
            pLogicalDevice->vkd.FreeCommandBuffers(device, pLogicalDevice->commandPool, 1, &uploadCommands);
            uploadCommands = VK_NULL_HANDLE;
            pLogicalDevice->vkd.DestroyFence(device, uploadFence, nullptr);
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (pLogicalDevice->vkd.CreateFence(device, &fenceInfo, nullptr, &uploadFence) != VK_SUCCESS)
                uploadFence = VK_NULL_HANDLE;
            return uploadFence ? Upload::Retry : Upload::Failed;
        }
        return Upload::Started;
    }
    void LutEffect::prepareImage(uint32_t imageIndex)
    {
        // The submit that last read this slot has finished: the app can present
        // image imageIndex again only after the presentation engine released it,
        // and that presentation waited on the semaphore of that submit.
        if (imageIndex >= paramSlotStale.size() || !paramSlotStale[imageIndex])
            return;
        std::copy(params.begin(), params.end(), paramSlots[imageIndex]);
        paramSlotStale[imageIndex] = false;
    }

    void LutEffect::updateGaze(std::chrono::steady_clock::time_point now)
    {
        if (!gazeRecord && now >= nextGazeOpen)
        {
            // Frame Dock creates it when gaze-following starts; until then try again once a second
            nextGazeOpen = now + std::chrono::seconds(1);
            const int fd = ::open(framegaze::path(), O_RDONLY | O_CLOEXEC);
            if (fd >= 0)
            {
                struct stat st{};
                if (::fstat(fd, &st) == 0 && static_cast<size_t>(st.st_size) >= sizeof(framegaze::Record))
                {
                    void* memory = ::mmap(nullptr, sizeof(framegaze::Record), PROT_READ, MAP_SHARED, fd, 0);
                    if (memory != MAP_FAILED)
                        gazeRecord = memory;
                }
                ::close(fd);
            }
        }
        framegaze::Sample sample;
        timespec mono{};
        clock_gettime(CLOCK_MONOTONIC, &mono);
        const uint64_t nowNs = static_cast<uint64_t>(mono.tv_sec) * 1000000000ull + static_cast<uint64_t>(mono.tv_nsec);
        const bool readOk = gazeRecord && framegaze::read(*static_cast<const framegaze::Record*>(gazeRecord), sample);
        // A read that keeps failing (no file, a writer stuck mid-write) ages the last point out like a stale one
        if (!readOk)
            sample = lastGaze;
        else
            lastGaze = sample;
        {
            // A point older than 200 ms (Frame Dock stopped, a stall) counts as no point: the lens centre again
            const bool fresh = sample.valid && nowNs >= sample.writtenNs && nowNs - sample.writtenNs < 200000000ull;
            const std::array<float, 5> next{{fresh ? 1.0f : 0.0f, fresh ? sample.leftU : 0.5f, fresh ? sample.leftV : 0.5f,
                                             fresh ? sample.rightU : 0.5f, fresh ? sample.rightV : 0.5f}};
            const float state = next[0] + debugState();
            if (state != params[3] || !std::equal(next.begin() + 1, next.end(), params.begin() + 4))
            {
                params[3] = state;
                std::copy(next.begin() + 1, next.end(), params.begin() + 4);
                std::fill(paramSlotStale.begin(), paramSlotStale.end(), true);
            }
        }
    }
} // namespace vkBasalt
