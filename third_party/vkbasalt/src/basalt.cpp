#include "vulkan_include.hpp"

#include <mutex>
#include <map>
#include <vector>
#include <unordered_map>
#include <iostream>
#include <string>
#include <memory>
#include <cstring>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>

#include "util.hpp"

#include "logical_device.hpp"
#include "logical_swapchain.hpp"

#include "image_view.hpp"
#include "sampler.hpp"
#include "framebuffer.hpp"
#include "descriptor_set.hpp"
#include "shader.hpp"
#include "graphics_pipeline.hpp"
#include "command_buffer.hpp"
#include "buffer.hpp"
#include "config.hpp"
#include "fake_swapchain.hpp"
#include "renderpass.hpp"
#include "format.hpp"
#include "logger.hpp"

#include "effect.hpp"
#include "effect_lut.hpp"
#include "effect_transfer.hpp"

#define VKBASALT_NAME "VK_LAYER_VKBASALT_post_processing"

#if defined(__GNUC__) && __GNUC__ >= 4
#define VK_BASALT_EXPORT __attribute__((visibility("default")))
#else
#error "Unsupported platform!"
#endif

namespace vkBasalt
{
    // This experiment must not alter SteamVR's helper processes or vrlink.
    const bool targetProcess = [] {
        char path[4096] = {};
        const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
        if (length <= 0) return false;
        path[length] = '\0';
        const char* basename = std::strrchr(path, '/');
        return std::strcmp(basename ? basename + 1 : path, "vrcompositor") == 0;
    }();

    std::shared_ptr<Config> pConfig = nullptr;

    Logger Logger::s_instance;

    // layer book-keeping information, to store dispatch tables by key
    std::unordered_map<void*, InstanceDispatch>                           instanceDispatchMap;
    std::unordered_map<void*, VkInstance>                                 instanceMap;
    std::unordered_map<void*, uint32_t>                                   instanceVersionMap;
    std::unordered_map<void*, std::shared_ptr<LogicalDevice>>             deviceMap;
    std::unordered_map<VkSwapchainKHR, std::shared_ptr<LogicalSwapchain>> swapchainMap;

    std::mutex globalLock;
#ifdef _GCC_
    using scoped_lock __attribute__((unused)) = std::lock_guard<std::mutex>;
#else
    using scoped_lock = std::lock_guard<std::mutex>;
#endif

    template<typename DispatchableType>
    void* GetKey(DispatchableType inst)
    {
        return *(void**) inst;
    }

    VkResult VKAPI_CALL vkBasalt_CreateInstance(const VkInstanceCreateInfo*  pCreateInfo,
                                                const VkAllocationCallbacks* pAllocator,
                                                VkInstance*                  pInstance)
    {
        VkLayerInstanceCreateInfo* layerCreateInfo = (VkLayerInstanceCreateInfo*) pCreateInfo->pNext;

        // step through the chain of pNext until we get to the link info
        while (layerCreateInfo
               && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO || layerCreateInfo->function != VK_LAYER_LINK_INFO))
        {
            layerCreateInfo = (VkLayerInstanceCreateInfo*) layerCreateInfo->pNext;
        }

        Logger::trace("vkCreateInstance");

        if (layerCreateInfo == nullptr)
        {
            // No loader instance create info
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        PFN_vkGetInstanceProcAddr gpa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        // move chain on for next layer
        layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

        PFN_vkCreateInstance createFunc = (PFN_vkCreateInstance) gpa(VK_NULL_HANDLE, "vkCreateInstance");
        if (!createFunc)
        {
            Logger::err("vkBasalt: failed to resolve vkCreateInstance from next layer");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkInstanceCreateInfo modifiedCreateInfo = *pCreateInfo;
        VkApplicationInfo    appInfo;
        if (modifiedCreateInfo.pApplicationInfo)
        {
            appInfo = *(modifiedCreateInfo.pApplicationInfo);
            if (appInfo.apiVersion < VK_API_VERSION_1_1)
            {
                appInfo.apiVersion = VK_API_VERSION_1_1;
            }
        }
        else
        {
            appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            appInfo.pNext              = nullptr;
            appInfo.pApplicationName   = nullptr;
            appInfo.applicationVersion = 0;
            appInfo.pEngineName        = nullptr;
            appInfo.engineVersion      = 0;
            appInfo.apiVersion         = VK_API_VERSION_1_1;
        }

        if (targetProcess)
            modifiedCreateInfo.pApplicationInfo = &appInfo;
        VkResult ret                        = createFunc(&modifiedCreateInfo, pAllocator, pInstance);
        if (ret != VK_SUCCESS)
            return ret;

        // fetch our own dispatch table for the functions we need, into the next layer
        InstanceDispatch dispatchTable;
        fillDispatchTableInstance(*pInstance, gpa, &dispatchTable);

        // store the table by key
        {
            scoped_lock l(globalLock);
            instanceDispatchMap[GetKey(*pInstance)] = dispatchTable;
            instanceMap[GetKey(*pInstance)]         = *pInstance;
            instanceVersionMap[GetKey(*pInstance)]  = modifiedCreateInfo.pApplicationInfo ? modifiedCreateInfo.pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
        }

        return ret;
    }

    void VKAPI_CALL vkBasalt_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator)
    {
        if (!instance)
            return;

        scoped_lock l(globalLock);

        Logger::trace("vkDestroyInstance");

        InstanceDispatch dispatchTable = instanceDispatchMap[GetKey(instance)];

        dispatchTable.DestroyInstance(instance, pAllocator);

        instanceDispatchMap.erase(GetKey(instance));
        instanceMap.erase(GetKey(instance));
        instanceVersionMap.erase(GetKey(instance));
    }

    VkResult VKAPI_CALL vkBasalt_CreateDevice(VkPhysicalDevice             physicalDevice,
                                              const VkDeviceCreateInfo*    pCreateInfo,
                                              const VkAllocationCallbacks* pAllocator,
                                              VkDevice*                    pDevice)
    {
        scoped_lock l(globalLock);
        Logger::trace("vkCreateDevice");
        VkLayerDeviceCreateInfo* layerCreateInfo = (VkLayerDeviceCreateInfo*) pCreateInfo->pNext;

        // step through the chain of pNext until we get to the link info
        while (layerCreateInfo
               && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO || layerCreateInfo->function != VK_LAYER_LINK_INFO))
        {
            layerCreateInfo = (VkLayerDeviceCreateInfo*) layerCreateInfo->pNext;
        }

        if (layerCreateInfo == nullptr)
        {
            // No loader instance create info
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        PFN_vkGetInstanceProcAddr gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        PFN_vkGetDeviceProcAddr   gdpa = layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
        // move chain on for next layer
        layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

        VkInstance instance = VK_NULL_HANDLE;
        auto instanceIt = instanceMap.find(GetKey(physicalDevice));
        if (instanceIt != instanceMap.end())
        {
            instance = instanceIt->second;
        }
        else if (!instanceMap.empty())
        {
            instance = instanceMap.begin()->second;
        }

        PFN_vkCreateDevice createFunc = nullptr;
        if (instance != VK_NULL_HANDLE)
        {
            createFunc = (PFN_vkCreateDevice) gipa(instance, "vkCreateDevice");
        }
        if (!createFunc)
        {
            createFunc = (PFN_vkCreateDevice) gipa(VK_NULL_HANDLE, "vkCreateDevice");
        }
        if (!createFunc)
        {
            Logger::err("vkBasalt: failed to resolve vkCreateDevice from next layer");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        if (!targetProcess)
        {
            VkResult ret = createFunc(physicalDevice, pCreateInfo, pAllocator, pDevice);
            if (ret != VK_SUCCESS) return ret;
            std::shared_ptr<LogicalDevice> pLogicalDevice(new LogicalDevice());
            pLogicalDevice->vki = instanceDispatchMap[GetKey(physicalDevice)];
            pLogicalDevice->device = *pDevice;
            pLogicalDevice->physicalDevice = physicalDevice;
            pLogicalDevice->instance = instanceMap[GetKey(physicalDevice)];
            pLogicalDevice->queue = VK_NULL_HANDLE;
            pLogicalDevice->commandPool = VK_NULL_HANDLE;
            pLogicalDevice->supportsMutableFormat = false;
            fillDispatchTableDevice(*pDevice, gdpa, &pLogicalDevice->vkd);
            deviceMap[GetKey(*pDevice)] = pLogicalDevice;
            return VK_SUCCESS;
        }

        // check and activate extentions
        uint32_t extensionCount = 0;

        instanceDispatchMap[GetKey(physicalDevice)].EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensionProperties(extensionCount);
        instanceDispatchMap[GetKey(physicalDevice)].EnumerateDeviceExtensionProperties(
            physicalDevice, nullptr, &extensionCount, extensionProperties.data());

        bool supportsMutableFormat = false;
        for (VkExtensionProperties properties : extensionProperties)
        {
            if (properties.extensionName == std::string("VK_KHR_swapchain_mutable_format"))
            {
                Logger::debug("device supports VK_KHR_swapchain_mutable_format");
                supportsMutableFormat = true;
                break;
            }
        }

        VkPhysicalDeviceProperties deviceProps;
        instanceDispatchMap[GetKey(physicalDevice)].GetPhysicalDeviceProperties(physicalDevice, &deviceProps);

        VkDeviceCreateInfo       modifiedCreateInfo = *pCreateInfo;
        std::vector<const char*> enabledExtensionNames;
        if (modifiedCreateInfo.enabledExtensionCount)
        {
            enabledExtensionNames = std::vector<const char*>(modifiedCreateInfo.ppEnabledExtensionNames,
                                                             modifiedCreateInfo.ppEnabledExtensionNames + modifiedCreateInfo.enabledExtensionCount);
        }

        if (supportsMutableFormat)
        {
            Logger::debug("activating mutable_format");
            addUniqueCString(enabledExtensionNames, "VK_KHR_swapchain_mutable_format");
        }
        if (deviceProps.apiVersion < VK_API_VERSION_1_2 || instanceVersionMap[GetKey(physicalDevice)] < VK_API_VERSION_1_2)
        {
            addUniqueCString(enabledExtensionNames, "VK_KHR_image_format_list");
        }
        modifiedCreateInfo.ppEnabledExtensionNames = enabledExtensionNames.data();
        modifiedCreateInfo.enabledExtensionCount   = enabledExtensionNames.size();

        // Active needed Features
        VkPhysicalDeviceFeatures deviceFeatures = {};
        if (modifiedCreateInfo.pEnabledFeatures)
        {
            deviceFeatures = *(modifiedCreateInfo.pEnabledFeatures);
        }
        deviceFeatures.shaderImageGatherExtended = VK_TRUE;
        modifiedCreateInfo.pEnabledFeatures      = &deviceFeatures;

        VkResult ret = createFunc(physicalDevice, &modifiedCreateInfo, pAllocator, pDevice);

        if (ret != VK_SUCCESS)
            return ret;

        std::shared_ptr<LogicalDevice> pLogicalDevice(new LogicalDevice());
        pLogicalDevice->vki                   = instanceDispatchMap[GetKey(physicalDevice)];
        pLogicalDevice->device                = *pDevice;
        pLogicalDevice->physicalDevice        = physicalDevice;
        pLogicalDevice->instance              = instanceMap[GetKey(physicalDevice)];
        pLogicalDevice->queue                 = VK_NULL_HANDLE;
        pLogicalDevice->queueFamilyIndex      = 0;
        pLogicalDevice->commandPool           = VK_NULL_HANDLE;
        pLogicalDevice->supportsMutableFormat = supportsMutableFormat;

        fillDispatchTableDevice(*pDevice, gdpa, &pLogicalDevice->vkd);

        uint32_t count;

        pLogicalDevice->vki.GetPhysicalDeviceQueueFamilyProperties(pLogicalDevice->physicalDevice, &count, nullptr);

        std::vector<VkQueueFamilyProperties> queueProperties(count);

        pLogicalDevice->vki.GetPhysicalDeviceQueueFamilyProperties(pLogicalDevice->physicalDevice, &count, queueProperties.data());
        for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++)
        {
            auto& queueInfo = pCreateInfo->pQueueCreateInfos[i];
            if ((queueProperties[queueInfo.queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            {
                pLogicalDevice->vkd.GetDeviceQueue(pLogicalDevice->device, queueInfo.queueFamilyIndex, 0, &pLogicalDevice->queue);

                VkCommandPoolCreateInfo commandPoolCreateInfo;
                commandPoolCreateInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                commandPoolCreateInfo.pNext            = nullptr;
                commandPoolCreateInfo.flags            = 0;
                commandPoolCreateInfo.queueFamilyIndex = queueInfo.queueFamilyIndex;

                Logger::debug("Found graphics capable queue");
                if (pLogicalDevice->vkd.CreateCommandPool(pLogicalDevice->device, &commandPoolCreateInfo, nullptr, &pLogicalDevice->commandPool)
                    != VK_SUCCESS)
                {
                    // FrameColor: its swapchains then show the original image (vkBasalt_GetSwapchainImagesKHR)
                    Logger::err("FrameColor: no command pool");
                    pLogicalDevice->commandPool = VK_NULL_HANDLE;
                }
                pLogicalDevice->queueFamilyIndex = queueInfo.queueFamilyIndex;

                initializeDispatchTable(pLogicalDevice->queue, pLogicalDevice->device);

                break;
            }
        }

        if (!pLogicalDevice->queue)
            Logger::err("Did not find a graphics queue!");

        deviceMap[GetKey(*pDevice)] = pLogicalDevice;

        return VK_SUCCESS;
    }

    void VKAPI_CALL vkBasalt_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
    {
        if (!device)
            return;

        scoped_lock l(globalLock);

        Logger::trace("vkDestroyDevice");

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        if (pLogicalDevice->commandPool != VK_NULL_HANDLE)
        {
            Logger::debug("DestroyCommandPool");
            pLogicalDevice->vkd.DestroyCommandPool(device, pLogicalDevice->commandPool, pAllocator);
        }

        pLogicalDevice->vkd.DestroyDevice(device, pAllocator);

        deviceMap.erase(GetKey(device));
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkBasalt_CreateSwapchainKHR(VkDevice                        device,
                                                               const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                               const VkAllocationCallbacks*    pAllocator,
                                                               VkSwapchainKHR*                 pSwapchain)
    {
        scoped_lock l(globalLock);

        Logger::trace("vkCreateSwapchainKHR");

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        if (!targetProcess)
            return pLogicalDevice->vkd.CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
        Logger::info("FrameColor vrcompositor swapchain " + std::to_string(pCreateInfo->imageExtent.width) + "x" + std::to_string(pCreateInfo->imageExtent.height) + " format=" + std::to_string(pCreateInfo->imageFormat) + " usage=" + std::to_string(pCreateInfo->imageUsage));

        VkSwapchainCreateInfoKHR modifiedCreateInfo = *pCreateInfo;

        VkFormat format = modifiedCreateInfo.imageFormat;

        VkFormat srgbFormat  = isSRGB(format) ? format : convertToSRGB(format);
        VkFormat unormFormat = isSRGB(format) ? convertToUNORM(format) : format;
        Logger::debug(std::to_string(srgbFormat) + " " + std::to_string(unormFormat));

        VkFormat formats[] = {unormFormat, srgbFormat};

        VkImageFormatListCreateInfoKHR imageFormatListCreateInfo;
        if (pLogicalDevice->supportsMutableFormat)
        {
            // FrameColor: only the layer's usage, as before. Adding the app's bits (so a passthrough swapchain, decided
            // later in vkBasalt_GetSwapchainImagesKHR, gets them) would also put them on every real image of the normal
            // path, where e.g. STORAGE can cost compression; the app's usage is logged above to decide that later
            modifiedCreateInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                            | VK_IMAGE_USAGE_SAMPLED_BIT; // we want to use the swapchain images as output of the graphics pipeline
            modifiedCreateInfo.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
            // TODO what if the application already uses multiple formats for the swapchain?

            imageFormatListCreateInfo.sType           = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR;
            imageFormatListCreateInfo.pNext           = modifiedCreateInfo.pNext;
            imageFormatListCreateInfo.viewFormatCount = (srgbFormat == unormFormat) ? 1 : 2;
            imageFormatListCreateInfo.pViewFormats    = formats;

            modifiedCreateInfo.pNext = &imageFormatListCreateInfo;
        }

        modifiedCreateInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;


        Logger::debug("format " + std::to_string(modifiedCreateInfo.imageFormat));
        std::shared_ptr<LogicalSwapchain> pLogicalSwapchain(new LogicalSwapchain());
        pLogicalSwapchain->pLogicalDevice      = pLogicalDevice;
        pLogicalSwapchain->swapchainCreateInfo = *pCreateInfo;
        pLogicalSwapchain->imageExtent         = modifiedCreateInfo.imageExtent;
        pLogicalSwapchain->format              = modifiedCreateInfo.imageFormat;
        pLogicalSwapchain->imageCount          = 0;

        VkResult result = pLogicalDevice->vkd.CreateSwapchainKHR(device, &modifiedCreateInfo, pAllocator, pSwapchain);
        if (result != VK_SUCCESS)
            return result;


        swapchainMap[*pSwapchain] = pLogicalSwapchain;

        return result;
    }

    // FrameColor (2026-11-13): the render size of a standalone game, for Frame Dock's Frame card. A VR game's eye
    // textures reach vrcompositor as 2D images of two layers imported from external memory (Eleven Table Tennis:
    // three 2400x2400 OPAQUE_FD images at launch, SteamVR recommending 1728x1728); overlays and dashboards are
    // one-layer dma-bufs, and SteamVR's own two-layer images are 512x512 or smaller (seen at start with no game,
    // 2026-11-14), so 1024 is the floor. The latest such image's size goes to $XDG_RUNTIME_DIR/framecolor/render-size as
    // "W H <unix seconds>" (temp file + rename). Once per created image, never per frame.
    namespace
    {
        void noteRenderSize(const VkImageCreateInfo* ci)
        {
            if (ci->imageType != VK_IMAGE_TYPE_2D || ci->arrayLayers != 2 || ci->extent.width < 1024
                || ci->extent.height < 1024)
                return;
            uint32_t external = 0;
            for (const VkBaseInStructure* n = static_cast<const VkBaseInStructure*>(ci->pNext); n; n = n->pNext)
            {
                if (n->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO)
                    external |= reinterpret_cast<const VkExternalMemoryImageCreateInfo*>(n)->handleTypes;
            }
            if (external == 0)
                return;
            const char* runtime = std::getenv("XDG_RUNTIME_DIR");
            if (!runtime || !runtime[0])
                return;
            const std::string dir  = std::string(runtime) + "/framecolor";
            const std::string path = dir + "/render-size";
            const std::string temp = path + ".tmp" + std::to_string(static_cast<int>(getpid()));
            mkdir(dir.c_str(), 0700);
            FILE* f = std::fopen(temp.c_str(), "w");
            if (!f)
                return;
            const bool written = std::fprintf(f, "%u %u %lld\n", ci->extent.width, ci->extent.height,
                                              static_cast<long long>(std::time(nullptr)))
                                 > 0;
            if (std::fclose(f) != 0 || !written || std::rename(temp.c_str(), path.c_str()) != 0)
                std::remove(temp.c_str());
        }
    } // namespace

    VKAPI_ATTR VkResult VKAPI_CALL vkBasalt_CreateImage(VkDevice                     device,
                                                        const VkImageCreateInfo*     pCreateInfo,
                                                        const VkAllocationCallbacks* pAllocator,
                                                        VkImage*                     pImage)
    {
        LogicalDevice* pLogicalDevice;
        {
            // Only the lookup: globalLock isn't recursive, and QueuePresentKHR holds it
            scoped_lock l(globalLock);
            auto found = deviceMap.find(GetKey(device));
            pLogicalDevice = found != deviceMap.end() ? found->second.get() : nullptr;
        }
        // A device that didn't come through our vkCreateDevice has no next function to call
        if (pLogicalDevice == nullptr)
            return VK_ERROR_INITIALIZATION_FAILED;
        VkResult result = pLogicalDevice->vkd.CreateImage(device, pCreateInfo, pAllocator, pImage);
        if (result == VK_SUCCESS && targetProcess)
            noteRenderSize(pCreateInfo);
        return result;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkBasalt_GetSwapchainImagesKHR(VkDevice       device,
                                                                  VkSwapchainKHR swapchain,
                                                                  uint32_t*      pCount,
                                                                  VkImage*       pSwapchainImages)
    {
        scoped_lock l(globalLock);
        Logger::trace("vkGetSwapchainImagesKHR " + std::to_string(*pCount));

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        if (!targetProcess)
            return pLogicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, pCount, pSwapchainImages);

        if (pSwapchainImages == nullptr)
        {
            return pLogicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, pCount, pSwapchainImages);
        }

        LogicalSwapchain* pLogicalSwapchain = swapchainMap[swapchain].get();

        // FrameColor: no effect on this swapchain (see below): the app uses its own images
        if (pLogicalSwapchain->passthrough)
            return pLogicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, pCount, pSwapchainImages);

        // If the images got already requested once, return them again instead of creating new images
        if (pLogicalSwapchain->fakeImages.size())
        {
            *pCount = std::min<uint32_t>(*pCount, pLogicalSwapchain->imageCount);
            std::memcpy(pSwapchainImages, pLogicalSwapchain->fakeImages.data(), sizeof(VkImage) * (*pCount));
            return *pCount < pLogicalSwapchain->imageCount ? VK_INCOMPLETE : VK_SUCCESS;
        }

        // Nothing to draw the effect with
        if (!pLogicalDevice->queue || !pLogicalDevice->commandPool)
        {
            Logger::err("FrameColor: no graphics queue or command pool, this swapchain shows the original image");
            pLogicalSwapchain->passthrough = true;
            return pLogicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, pCount, pSwapchainImages);
        }

        pLogicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, &pLogicalSwapchain->imageCount, nullptr);
        pLogicalSwapchain->images.resize(pLogicalSwapchain->imageCount);
        pLogicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, &pLogicalSwapchain->imageCount, pLogicalSwapchain->images.data());

        // FrameColor: a Vulkan call that fails while the effect is set up (out of memory, say) throws VulkanFailure.
        // Nothing has been handed to the app yet, so it gets the swapchain's own images instead and its presents go
        // down unchanged: no colour on this swapchain rather than drawing with null handles inside vrcompositor. A step
        // cut short frees what it had made itself (the helpers, LutEffect); what the steps before it made is freed here
        try
        {
            // FrameColor only uses one LUT effect. Keep the transfer stage for
            // devices without mutable swapchain images.
            uint32_t fakeImageCount = pLogicalSwapchain->imageCount * (1 + !pLogicalDevice->supportsMutableFormat);

            pLogicalSwapchain->fakeImages =
                createFakeSwapchainImages(pLogicalDevice, pLogicalSwapchain->swapchainCreateInfo, fakeImageCount, pLogicalSwapchain->fakeImageMemory);
            Logger::debug("created fake swapchain images");

            VkFormat unormFormat = convertToUNORM(pLogicalSwapchain->format);
            std::vector<VkImage> firstImages(pLogicalSwapchain->fakeImages.begin(),
                                             pLogicalSwapchain->fakeImages.begin() + pLogicalSwapchain->imageCount);
            std::vector<VkImage> secondImages = pLogicalDevice->supportsMutableFormat
                ? pLogicalSwapchain->images
                : std::vector<VkImage>(pLogicalSwapchain->fakeImages.end() - pLogicalSwapchain->imageCount,
                                       pLogicalSwapchain->fakeImages.end());
            pLogicalSwapchain->effects.push_back(std::shared_ptr<Effect>(new LutEffect(
                pLogicalDevice, unormFormat, pLogicalSwapchain->imageExtent, firstImages, secondImages, pConfig.get())));

            if (!pLogicalDevice->supportsMutableFormat)
            {
                pLogicalSwapchain->effects.push_back(std::shared_ptr<Effect>(new TransferEffect(
                    pLogicalDevice,
                    pLogicalSwapchain->format,
                    pLogicalSwapchain->imageExtent,
                    std::vector<VkImage>(pLogicalSwapchain->fakeImages.end() - pLogicalSwapchain->imageCount, pLogicalSwapchain->fakeImages.end()),
                    pLogicalSwapchain->images,
                    pConfig.get())));
            }

            Logger::debug("effect count: " + std::to_string(pLogicalSwapchain->effects.size()));

            pLogicalSwapchain->commandBuffersEffect = allocateCommandBuffer(pLogicalDevice, pLogicalSwapchain->imageCount);
            Logger::debug("allocated ComandBuffers " + std::to_string(pLogicalSwapchain->commandBuffersEffect.size()) + " for swapchain "
                          + convertToString(swapchain));

            writeCommandBuffers(pLogicalDevice, pLogicalSwapchain->effects, pLogicalSwapchain->commandBuffersEffect);
            Logger::debug("wrote CommandBuffers");

            pLogicalSwapchain->semaphores = createSemaphores(pLogicalDevice, pLogicalSwapchain->imageCount);
            Logger::debug("created semaphores");
            for (unsigned int i = 0; i < pLogicalSwapchain->imageCount; i++)
            {
                Logger::debug(std::to_string(i) + " written commandbuffer " + convertToString(pLogicalSwapchain->commandBuffersEffect[i]));
            }
        }
        catch (const std::exception& error)
        {
            Logger::err(std::string("FrameColor: colour setup failed (") + error.what() + "), this swapchain shows the original image"
                        + " (app usage " + std::to_string(pLogicalSwapchain->swapchainCreateInfo.imageUsage) + ")");
            pLogicalSwapchain->destroy();
            pLogicalSwapchain->passthrough = true;
            return pLogicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, pCount, pSwapchainImages);
        }
        Logger::trace("vkGetSwapchainImagesKHR");

        *pCount = std::min<uint32_t>(*pCount, pLogicalSwapchain->imageCount);
        std::memcpy(pSwapchainImages, pLogicalSwapchain->fakeImages.data(), sizeof(VkImage) * (*pCount));
        return *pCount < pLogicalSwapchain->imageCount ? VK_INCOMPLETE : VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkBasalt_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
    {
        scoped_lock l(globalLock);
        if (!targetProcess)
            return deviceMap[GetKey(queue)]->vkd.QueuePresentKHR(queue, pPresentInfo);
        LogicalDevice* pLogicalDevice = deviceMap[GetKey(queue)].get();

        static bool reportedPresent = false;
        if (!reportedPresent) {
            Logger::info("FrameColor vrcompositor first intercepted present");
            // The effect is submitted to the layer's own queue (queue 0 of the first graphics family vrcompositor asked
            // for, the family of the layer's command pool), not to this one: said once if they differ
            if (queue != pLogicalDevice->queue)
                Logger::warn("FrameColor: vrcompositor presents on another queue than the layer submits to");
            reportedPresent = true;
        }

        std::vector<VkSemaphore> presentSemaphores;
        presentSemaphores.reserve(pPresentInfo->swapchainCount);

        std::vector<VkPipelineStageFlags> waitStages(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

        // FrameColor: the app's wait semaphores go to the first submit; a passthrough swapchain has none
        bool appWaited = false;
        for (unsigned int i = 0; i < (*pPresentInfo).swapchainCount; i++)
        {
            uint32_t          index             = (*pPresentInfo).pImageIndices[i];
            VkSwapchainKHR    swapchain         = (*pPresentInfo).pSwapchains[i];
            LogicalSwapchain* pLogicalSwapchain = swapchainMap[swapchain].get();
            if (pLogicalSwapchain->passthrough)
                continue;

            for (auto& effect : pLogicalSwapchain->effects)
            {
                effect->updateEffect();
                effect->prepareImage(index);
            }

            VkSubmitInfo submitInfo;
            submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.pNext              = nullptr;
            submitInfo.waitSemaphoreCount = !appWaited ? pPresentInfo->waitSemaphoreCount : 0;
            submitInfo.pWaitSemaphores    = !appWaited ? pPresentInfo->pWaitSemaphores : nullptr;
            submitInfo.pWaitDstStageMask  = !appWaited ? waitStages.data() : nullptr;
            appWaited                     = true;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &(pLogicalSwapchain->commandBuffersEffect[index]);
            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores    = &(pLogicalSwapchain->semaphores[index]);

            presentSemaphores.push_back(pLogicalSwapchain->semaphores[index]);

            VkResult vr = pLogicalDevice->vkd.QueueSubmit(pLogicalDevice->queue, 1, &submitInfo, VK_NULL_HANDLE);

            if (vr != VK_SUCCESS)
            {
                return vr;
            }
        }

        // No effect on any of them: the present goes down as it came. Otherwise it waits on the effect submits only, and
        // those waited on the app's semaphores (so a passthrough swapchain's image is ready too)
        if (presentSemaphores.empty())
            return pLogicalDevice->vkd.QueuePresentKHR(queue, pPresentInfo);

        VkPresentInfoKHR presentInfo   = *pPresentInfo;
        presentInfo.waitSemaphoreCount = presentSemaphores.size();
        presentInfo.pWaitSemaphores    = presentSemaphores.data();

        return pLogicalDevice->vkd.QueuePresentKHR(queue, &presentInfo);
    }

    VKAPI_ATTR void VKAPI_CALL vkBasalt_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator)
    {
        if (!swapchain)
            return;

        scoped_lock l(globalLock);
        // we need to delete the infos of the oldswapchain

        Logger::trace("vkDestroySwapchainKHR " + convertToString(swapchain));
        if (!targetProcess)
        {
            deviceMap[GetKey(device)]->vkd.DestroySwapchainKHR(device, swapchain, pAllocator);
            return;
        }
        swapchainMap[swapchain]->destroy();
        swapchainMap.erase(swapchain);
        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();

        pLogicalDevice->vkd.DestroySwapchainKHR(device, swapchain, pAllocator);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Enumeration function

    VkResult VKAPI_CALL vkBasalt_EnumerateInstanceLayerProperties(uint32_t* pPropertyCount, VkLayerProperties* pProperties)
    {
        if (pPropertyCount)
            *pPropertyCount = 1;

        if (pProperties)
        {
            std::strcpy(pProperties->layerName, VKBASALT_NAME);
            std::strcpy(pProperties->description, "a post processing layer");
            pProperties->implementationVersion = 1;
            pProperties->specVersion           = VK_MAKE_VERSION(1, 2, 0);
        }

        return VK_SUCCESS;
    }

    VkResult VKAPI_CALL vkBasalt_EnumerateDeviceLayerProperties(VkPhysicalDevice   physicalDevice,
                                                                uint32_t*          pPropertyCount,
                                                                VkLayerProperties* pProperties)
    {
        return vkBasalt_EnumerateInstanceLayerProperties(pPropertyCount, pProperties);
    }

    VkResult VKAPI_CALL vkBasalt_EnumerateInstanceExtensionProperties(const char*            pLayerName,
                                                                      uint32_t*              pPropertyCount,
                                                                      VkExtensionProperties* pProperties)
    {
        if (pLayerName == NULL || std::strcmp(pLayerName, VKBASALT_NAME))
        {
            return VK_ERROR_LAYER_NOT_PRESENT;
        }

        // don't expose any extensions
        if (pPropertyCount)
        {
            *pPropertyCount = 0;
        }
        return VK_SUCCESS;
    }

    VkResult VKAPI_CALL vkBasalt_EnumerateDeviceExtensionProperties(VkPhysicalDevice       physicalDevice,
                                                                    const char*            pLayerName,
                                                                    uint32_t*              pPropertyCount,
                                                                    VkExtensionProperties* pProperties)
    {
        // pass through any queries that aren't to us
        if (pLayerName == NULL || std::strcmp(pLayerName, VKBASALT_NAME))
        {
            if (physicalDevice == VK_NULL_HANDLE)
            {
                return VK_SUCCESS;
            }

            scoped_lock l(globalLock);
            return instanceDispatchMap[GetKey(physicalDevice)].EnumerateDeviceExtensionProperties(
                physicalDevice, pLayerName, pPropertyCount, pProperties);
        }

        // don't expose any extensions
        if (pPropertyCount)
        {
            *pPropertyCount = 0;
        }
        return VK_SUCCESS;
    }
} // namespace vkBasalt

extern "C"
{ // these are the entry points for the layer, so they need to be c-linkeable

    VK_BASALT_EXPORT PFN_vkVoidFunction VKAPI_CALL vkBasalt_GetDeviceProcAddr(VkDevice device, const char* pName);
    VK_BASALT_EXPORT PFN_vkVoidFunction VKAPI_CALL vkBasalt_GetInstanceProcAddr(VkInstance instance, const char* pName);

#define GETPROCADDR(func) \
    if (!std::strcmp(pName, "vk" #func)) \
        return (PFN_vkVoidFunction) &vkBasalt::vkBasalt_##func;
    /*
    Return our funktions for the funktions we want to intercept
    the macro takes the name and returns our vkBasalt_##func, if the name is equal
    */

    // vkGetDeviceProcAddr needs to behave like vkGetInstanceProcAddr thanks to some games
#define INTERCEPT_CALLS \
    /* instance chain functions we intercept */ \
    if (!std::strcmp(pName, "vkGetInstanceProcAddr")) \
        return (PFN_vkVoidFunction) &vkBasalt_GetInstanceProcAddr; \
    GETPROCADDR(EnumerateInstanceLayerProperties); \
    GETPROCADDR(EnumerateInstanceExtensionProperties); \
    GETPROCADDR(CreateInstance); \
    GETPROCADDR(DestroyInstance); \
\
    /* device chain functions we intercept*/ \
    if (!std::strcmp(pName, "vkGetDeviceProcAddr")) \
        return (PFN_vkVoidFunction) &vkBasalt_GetDeviceProcAddr; \
    GETPROCADDR(EnumerateDeviceLayerProperties); \
    GETPROCADDR(EnumerateDeviceExtensionProperties); \
    GETPROCADDR(CreateDevice); \
    GETPROCADDR(DestroyDevice); \
    GETPROCADDR(CreateSwapchainKHR); \
    GETPROCADDR(CreateImage); \
    GETPROCADDR(GetSwapchainImagesKHR); \
    GETPROCADDR(QueuePresentKHR); \
    GETPROCADDR(DestroySwapchainKHR); \
\

    VK_BASALT_EXPORT PFN_vkVoidFunction VKAPI_CALL vkBasalt_GetDeviceProcAddr(VkDevice device, const char* pName)
    {
        if (vkBasalt::pConfig == nullptr)
        {
            vkBasalt::pConfig = std::shared_ptr<vkBasalt::Config>(new vkBasalt::Config());
        }

        INTERCEPT_CALLS

        if (device == VK_NULL_HANDLE)
        {
            return nullptr;
        }

        {
            vkBasalt::scoped_lock l(vkBasalt::globalLock);
            auto it = vkBasalt::deviceMap.find(vkBasalt::GetKey(device));
            if (it == vkBasalt::deviceMap.end() || !it->second)
            {
                return nullptr;
            }
            return it->second->vkd.GetDeviceProcAddr(device, pName);
        }
    }

    VK_BASALT_EXPORT PFN_vkVoidFunction VKAPI_CALL vkBasalt_GetInstanceProcAddr(VkInstance instance, const char* pName)
    {
        if (vkBasalt::pConfig == nullptr)
        {
            vkBasalt::pConfig = std::shared_ptr<vkBasalt::Config>(new vkBasalt::Config());
        }

        INTERCEPT_CALLS

        if (instance == VK_NULL_HANDLE)
        {
            return nullptr;
        }

        {
            vkBasalt::scoped_lock l(vkBasalt::globalLock);
            auto it = vkBasalt::instanceDispatchMap.find(vkBasalt::GetKey(instance));
            if (it == vkBasalt::instanceDispatchMap.end())
            {
                return nullptr;
            }
            return it->second.GetInstanceProcAddr(instance, pName);
        }
    }

} // extern "C"
