// The Vulkan layer's effect setup when a Vulkan call fails (third_party/vkbasalt/src/basalt.cpp and the effect). It
// runs inside vrcompositor: a failed call must never throw out of the layer or leak; that swapchain goes on without
// the effect (passthrough) and gets its own images. No GPU: the device functions are fakes that count every handle,
// and the Nth create / allocate / map / bind / begin / end call fails, for every N until the setup goes through.
// The layer only acts in a process called vrcompositor (/proc/self/exe), so this test is built under that name.
#include "config.hpp"
#include "logical_device.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace vkBasalt {
// basalt.cpp's book-keeping and entry points
extern std::shared_ptr<Config> pConfig;
extern std::unordered_map<void*, std::shared_ptr<LogicalDevice>> deviceMap;
VkResult VKAPI_CALL vkBasalt_CreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*,
                                                VkSwapchainKHR*);
VkResult VKAPI_CALL vkBasalt_GetSwapchainImagesKHR(VkDevice, VkSwapchainKHR, uint32_t*, VkImage*);
void VKAPI_CALL vkBasalt_DestroySwapchainKHR(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
}  // namespace vkBasalt

namespace {

int failures = 0;

#define CHECK(expr)                                             \
    do {                                                        \
        if (!(expr)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr); \
            ++failures;                                         \
        }                                                       \
    } while (false)

// ---- The fake device: every live handle with its kind; a free of anything else is a bad free
std::map<uint64_t, std::string> live;
std::set<uint64_t> mapped;
int badFrees = 0, calls = 0, failAt = 0, swapchainsDestroyed = 0;
bool failed = false;
uint64_t nextHandle = 0x1000;
VkImageUsageFlags createdUsage = 0;
unsigned char mapping[1 << 20];  // what every MapMemory returns (the LUT staging buffer is 33³ × 4)
struct Dispatchable { void* key; };
Dispatchable deviceObject{&deviceObject}, queueObject{&deviceObject};
const VkImage realImages[3] = {reinterpret_cast<VkImage>(0x11), reinterpret_cast<VkImage>(0x12),
                               reinterpret_cast<VkImage>(0x13)};

/** @return VK_SUCCESS, or the injected failure for call number failAt */
VkResult step() {
    if (++calls != failAt) return VK_SUCCESS;
    failed = true;
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}
template <class H> VkResult make(const char* kind, H* out) {
    const VkResult result = step();
    if (result != VK_SUCCESS) {
        *out = reinterpret_cast<H>(uintptr_t{0xBAD});  // a failed call leaves its output undefined
        return result;
    }
    live[nextHandle] = kind;
    *out = reinterpret_cast<H>(uintptr_t{nextHandle++});
    return VK_SUCCESS;
}
template <class H> void drop(const char* kind, H handle) {
    if (!handle) return;
    const auto it = live.find(reinterpret_cast<uintptr_t>(handle));
    if (it == live.end() || it->second != kind) {
        std::printf("  bad free of %s %#lx (call %d failed)\n", kind, (unsigned long) reinterpret_cast<uintptr_t>(handle), failAt);
        ++badFrees;
        return;
    }
    live.erase(it);
}

#define MAKE(Kind, Info)                                                                                       \
    VKAPI_ATTR VkResult VKAPI_CALL Create##Kind(VkDevice, const Info*, const VkAllocationCallbacks*, Vk##Kind* out) { \
        return make(#Kind, out);                                                                               \
    }                                                                                                          \
    VKAPI_ATTR void VKAPI_CALL Destroy##Kind(VkDevice, Vk##Kind handle, const VkAllocationCallbacks*) { drop(#Kind, handle); }
MAKE(Buffer, VkBufferCreateInfo)
MAKE(DescriptorPool, VkDescriptorPoolCreateInfo)
MAKE(DescriptorSetLayout, VkDescriptorSetLayoutCreateInfo)
MAKE(Fence, VkFenceCreateInfo)
MAKE(Framebuffer, VkFramebufferCreateInfo)
MAKE(Image, VkImageCreateInfo)
MAKE(ImageView, VkImageViewCreateInfo)
MAKE(PipelineLayout, VkPipelineLayoutCreateInfo)
MAKE(RenderPass, VkRenderPassCreateInfo)
MAKE(Sampler, VkSamplerCreateInfo)
MAKE(Semaphore, VkSemaphoreCreateInfo)
MAKE(ShaderModule, VkShaderModuleCreateInfo)
#undef MAKE

VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo*,
                                                       const VkAllocationCallbacks*, VkPipeline* out) {
    return make("Pipeline", out);
}
VKAPI_ATTR void VKAPI_CALL DestroyPipeline(VkDevice, VkPipeline handle, const VkAllocationCallbacks*) { drop("Pipeline", handle); }
VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory* out) {
    return make("Memory", out);
}
VKAPI_ATTR void VKAPI_CALL FreeMemory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks*) {
    mapped.erase(reinterpret_cast<uintptr_t>(memory));
    drop("Memory", memory);
}
VKAPI_ATTR VkResult VKAPI_CALL MapMemory(VkDevice, VkDeviceMemory memory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void** data) {
    const VkResult result = step();
    if (result == VK_SUCCESS) {
        CHECK(live.count(reinterpret_cast<uintptr_t>(memory)) && !mapped.count(reinterpret_cast<uintptr_t>(memory)));
        mapped.insert(reinterpret_cast<uintptr_t>(memory));
        *data = mapping;
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL UnmapMemory(VkDevice, VkDeviceMemory memory) {
    if (!mapped.erase(reinterpret_cast<uintptr_t>(memory))) ++badFrees;
}
VKAPI_ATTR VkResult VKAPI_CALL AllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* out) {
    const VkResult result = step();
    for (uint32_t i = 0; i < info->commandBufferCount; ++i) {
        out[i] = VK_NULL_HANDLE;  // as Vulkan says for a failed allocation
        if (result != VK_SUCCESS) continue;
        out[i] = reinterpret_cast<VkCommandBuffer>(new Dispatchable{nullptr});
        live[reinterpret_cast<uintptr_t>(out[i])] = "CommandBuffer";
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL FreeCommandBuffers(VkDevice, VkCommandPool, uint32_t count, const VkCommandBuffer* buffers) {
    for (uint32_t i = 0; i < count; ++i) {
        const bool known = buffers[i] && live.count(reinterpret_cast<uintptr_t>(buffers[i]));
        drop("CommandBuffer", buffers[i]);
        if (known) delete reinterpret_cast<Dispatchable*>(buffers[i]);
    }
}
VKAPI_ATTR VkResult VKAPI_CALL AllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* out) {
    const VkResult result = step();  // freed with their pool
    for (uint32_t i = 0; i < info->descriptorSetCount; ++i) out[i] = reinterpret_cast<VkDescriptorSet>(uintptr_t{0x5E7});
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) { return step(); }
VKAPI_ATTR VkResult VKAPI_CALL BindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) { return step(); }
VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*) { return step(); }
VKAPI_ATTR VkResult VKAPI_CALL EndCommandBuffer(VkCommandBuffer) { return step(); }
VKAPI_ATTR void VKAPI_CALL GetBufferMemoryRequirements(VkDevice, VkBuffer, VkMemoryRequirements* out) { *out = {256, 256, 1}; }
VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements(VkDevice, VkImage, VkMemoryRequirements* out) { *out = {4096, 256, 1}; }
VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSets(VkDevice, uint32_t, const VkWriteDescriptorSet*, uint32_t, const VkCopyDescriptorSet*) {}
VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
                                              uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t,
                                              const VkImageMemoryBarrier*) {}
VKAPI_ATTR void VKAPI_CALL CmdCopyBufferToImage(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout, uint32_t, const VkBufferImageCopy*) {}
VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass(VkCommandBuffer, const VkRenderPassBeginInfo*, VkSubpassContents) {}
VKAPI_ATTR void VKAPI_CALL CmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t,
                                                 const VkDescriptorSet*, uint32_t, const uint32_t*) {}
VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}
VKAPI_ATTR void VKAPI_CALL CmdDraw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t) {}
VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass(VkCommandBuffer) {}
VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL QueueWaitIdle(VkQueue) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL WaitForFences(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks*,
                                                  VkSwapchainKHR* out) {
    createdUsage = info->imageUsage;
    *out = reinterpret_cast<VkSwapchainKHR>(uintptr_t{0x5C});
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*) { ++swapchainsDestroyed; }
VKAPI_ATTR VkResult VKAPI_CALL GetSwapchainImagesKHR(VkDevice, VkSwapchainKHR, uint32_t* count, VkImage* images) {
    if (images)
        for (uint32_t i = 0; i < *count && i < 3; ++i) images[i] = realImages[i];
    const VkResult result = images && *count < 3 ? VK_INCOMPLETE : VK_SUCCESS;
    *count = images && *count < 3 ? *count : 3;
    return result;
}
VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceMemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* out) {
    *out = {};
    out->memoryTypeCount = 1;
    out->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                        | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}
std::shared_ptr<vkBasalt::LogicalDevice> fakeDevice() {
    auto device = std::make_shared<vkBasalt::LogicalDevice>();
    auto& d = device->vkd;
#define FAKE(name) d.name = name;
    FAKE(AllocateCommandBuffers) FAKE(AllocateDescriptorSets) FAKE(AllocateMemory) FAKE(BeginCommandBuffer)
    FAKE(BindBufferMemory) FAKE(BindImageMemory) FAKE(CmdBeginRenderPass) FAKE(CmdBindDescriptorSets) FAKE(CmdBindPipeline)
    FAKE(CmdCopyBufferToImage) FAKE(CmdDraw) FAKE(CmdEndRenderPass) FAKE(CmdPipelineBarrier) FAKE(CreateBuffer)
    FAKE(CreateDescriptorPool) FAKE(CreateDescriptorSetLayout) FAKE(CreateFence) FAKE(CreateFramebuffer)
    FAKE(CreateGraphicsPipelines) FAKE(CreateImage) FAKE(CreateImageView) FAKE(CreatePipelineLayout) FAKE(CreateRenderPass)
    FAKE(CreateSampler) FAKE(CreateSemaphore) FAKE(CreateShaderModule) FAKE(CreateSwapchainKHR) FAKE(DestroyBuffer)
    FAKE(DestroyDescriptorPool) FAKE(DestroyDescriptorSetLayout) FAKE(DestroyFence) FAKE(DestroyFramebuffer)
    FAKE(DestroyImage) FAKE(DestroyImageView) FAKE(DestroyPipeline) FAKE(DestroyPipelineLayout) FAKE(DestroyRenderPass)
    FAKE(DestroySampler) FAKE(DestroySemaphore) FAKE(DestroyShaderModule) FAKE(DestroySwapchainKHR) FAKE(EndCommandBuffer)
    FAKE(FreeCommandBuffers) FAKE(FreeMemory) FAKE(GetBufferMemoryRequirements) FAKE(GetImageMemoryRequirements)
    FAKE(GetSwapchainImagesKHR) FAKE(MapMemory) FAKE(QueueSubmit) FAKE(QueueWaitIdle) FAKE(UnmapMemory)
    FAKE(UpdateDescriptorSets) FAKE(WaitForFences)
#undef FAKE
    device->vki.GetPhysicalDeviceMemoryProperties = GetPhysicalDeviceMemoryProperties;
    device->device = reinterpret_cast<VkDevice>(&deviceObject);
    device->physicalDevice = reinterpret_cast<VkPhysicalDevice>(&deviceObject);
    device->queue = reinterpret_cast<VkQueue>(&queueObject);
    device->commandPool = reinterpret_cast<VkCommandPool>(uintptr_t{0xC0});
    device->queueFamilyIndex = 0;
    device->supportsMutableFormat = true;
    return device;
}

}  // namespace

int main() {
    ::unsetenv("VKBASALT_CONFIG_FILE");  // no lutFile: the layer's identity cube (33³), so the live-update buffer too
    vkBasalt::pConfig = std::make_shared<vkBasalt::Config>();
    const VkDevice device = reinterpret_cast<VkDevice>(&deviceObject);
    int passthroughRuns = 0, degradedRuns = 0;
    for (failAt = 1;; ++failAt) {
        calls = 0;
        failed = false;
        swapchainsDestroyed = 0;
        vkBasalt::deviceMap[deviceObject.key] = fakeDevice();

        VkSwapchainCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
        info.imageExtent = {64, 64};
        info.imageArrayLayers = 1;
        // The real images get the layer's usage only (the app's own bits stay on the fake images)
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        CHECK(vkBasalt::vkBasalt_CreateSwapchainKHR(device, &info, nullptr, &swapchain) == VK_SUCCESS);
        CHECK(createdUsage == (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                               | VK_IMAGE_USAGE_TRANSFER_DST_BIT));

        uint32_t count = 0;
        CHECK(vkBasalt::vkBasalt_GetSwapchainImagesKHR(device, swapchain, &count, nullptr) == VK_SUCCESS && count == 3);
        VkImage images[3] = {};
        VkResult result = VK_ERROR_UNKNOWN;
        try {
            result = vkBasalt::vkBasalt_GetSwapchainImagesKHR(device, swapchain, &count, images);
        } catch (...) {
            std::printf("FAIL: an exception left the layer (call %d failed)\n", failAt);
            ++failures;
        }
        CHECK(result == VK_SUCCESS && count == 3);
        const bool real = images[0] == realImages[0] && images[1] == realImages[1] && images[2] == realImages[2];
        bool fake = true;
        for (VkImage image : images) fake = fake && live.count(reinterpret_cast<uintptr_t>(image)) && live[reinterpret_cast<uintptr_t>(image)] == "Image";
        if (real) {
            // Left without the effect: nothing of the layer's is left, and the images stay the same on the next ask
            CHECK(failed);
            CHECK(live.empty() && mapped.empty());
            ++passthroughRuns;
            VkImage again[3] = {};
            CHECK(vkBasalt::vkBasalt_GetSwapchainImagesKHR(device, swapchain, &count, again) == VK_SUCCESS && again[2] == realImages[2]);
        } else {
            CHECK(fake);
            if (failed) ++degradedRuns;  // a failure the effect works around (the live-update buffer, its fence)
        }
        vkBasalt::vkBasalt_DestroySwapchainKHR(device, swapchain, nullptr);
        CHECK(swapchainsDestroyed == 1);
        CHECK(live.empty() && mapped.empty());
        CHECK(badFrees == 0);
        if (!live.empty()) std::printf("  %zu handles left after call %d failed, first a %s\n", live.size(), failAt, live.begin()->second.c_str());
        live.clear();
        mapped.clear();
        badFrees = 0;
        vkBasalt::deviceMap.clear();
        if (!failed) break;  // the whole setup went through: every call has failed once
    }
    std::printf("%d calls; %d failures left the swapchain without the effect, %d were worked around\n", failAt - 1,
                passthroughRuns, degradedRuns);
    CHECK(failAt > 40 && passthroughRuns > 30 && degradedRuns >= 5);
    if (failures) std::printf("%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
