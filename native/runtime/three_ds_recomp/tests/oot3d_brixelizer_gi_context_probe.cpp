// GPU probe: FidelityFX Brixelizer and Brixelizer GI contexts on the local
// Vulkan device through the renderer's FidelityFX backend. Needs no game
// data. Exit 0 = both contexts created and destroyed; 77 = no Vulkan device.
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_brixelizer.h>
#include <FidelityFX/host/ffx_brixelizergi.h>

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {

[[noreturn]] void Fail(const char* message, long code = 0) {
    std::fprintf(stderr, "oot3d_brixelizer_gi_context_probe: %s (%ld)\n", message, code);
    std::exit(1);
}

struct Device {
    VkInstance Instance = VK_NULL_HANDLE;
    VkPhysicalDevice Physical = VK_NULL_HANDLE;
    VkDevice Handle = VK_NULL_HANDLE;
    ~Device() {
        if (Handle != VK_NULL_HANDLE) vkDestroyDevice(Handle, nullptr);
        if (Instance != VK_NULL_HANDLE) vkDestroyInstance(Instance, nullptr);
    }
};

bool CreateDevice(Device& device) {
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "oot3d_brixelizer_gi_context_probe";
    application.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &application;
    if (vkCreateInstance(&instanceInfo, nullptr, &device.Instance) != VK_SUCCESS) return false;
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(device.Instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physical(count);
    vkEnumeratePhysicalDevices(device.Instance, &count, physical.data());
    for (VkPhysicalDevice candidate : physical) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3) continue;
        device.Physical = candidate;
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) break;
    }
    if (device.Physical == VK_NULL_HANDLE) return false;
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device.Physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device.Physical, &familyCount, families.data());
    uint32_t family = familyCount;
    for (uint32_t i = 0; i < familyCount; ++i) {
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            family = i;
            break;
        }
    }
    if (family == familyCount) return false;
    // Enable every supported core feature: the backend selects its shader
    // variants (16-bit, wave64) from what the device reports.
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features features11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &features11;
    features11.pNext = &features12;
    features12.pNext = &features13;
    vkGetPhysicalDeviceFeatures2(device.Physical, &features);
    const float priority = 1.0F;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.pNext = &features;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queue;
    return vkCreateDevice(device.Physical, &deviceInfo, nullptr, &device.Handle) == VK_SUCCESS;
}

} // namespace

int main() {
    Device device;
    if (!CreateDevice(device)) {
        std::puts("oot3d_brixelizer_gi_context_probe: skipped, no Vulkan 1.3 device");
        return 77;
    }
    constexpr size_t contexts = FFX_BRIXELIZER_CONTEXT_COUNT + FFX_BRIXELIZER_GI_CONTEXT_COUNT;
    std::vector<unsigned char> scratch(ffxGetScratchMemorySizeVK(device.Physical, contexts));
    VkDeviceContext deviceContext{device.Handle, device.Physical, vkGetDeviceProcAddr};
    FfxInterface backend{};
    FfxErrorCode error = ffxGetInterfaceVK(&backend, ffxGetDeviceVK(&deviceContext), scratch.data(),
                                           scratch.size(), contexts);
    if (error != FFX_OK) Fail("backend interface", error);

    // The Brixelizer context is about 24 MB of opaque state.
    auto brixelizer = std::make_unique<FfxBrixelizerContext>();
    FfxBrixelizerContextDescription description{};
    description.numCascades = 4;
    float voxelSize = 0.25F;
    for (uint32_t i = 0; i < description.numCascades; ++i) {
        description.cascadeDescs[i].flags =
            static_cast<FfxBrixelizerCascadeFlag>(FFX_BRIXELIZER_CASCADE_STATIC | FFX_BRIXELIZER_CASCADE_DYNAMIC);
        description.cascadeDescs[i].voxelSize = voxelSize;
        voxelSize *= 2.0F;
    }
    description.backendInterface = backend;
    error = ffxBrixelizerContextCreate(&description, brixelizer.get());
    if (error != FFX_OK) Fail("Brixelizer context", error);

    auto gi = std::make_unique<FfxBrixelizerGIContext>();
    FfxBrixelizerGIContextDescription giDescription{};
    giDescription.flags = FFX_BRIXELIZER_GI_FLAG_DEPTH_INVERTED;
    giDescription.internalResolution = FFX_BRIXELIZER_GI_INTERNAL_RESOLUTION_50_PERCENT;
    giDescription.displaySize = {1280, 720};
    giDescription.backendInterface = backend;
    error = ffxBrixelizerGIContextCreate(gi.get(), &giDescription);
    if (error != FFX_OK) Fail("Brixelizer GI context", error);

    if (ffxBrixelizerGIContextDestroy(gi.get()) != FFX_OK) Fail("Brixelizer GI destroy");
    if (ffxBrixelizerContextDestroy(brixelizer.get()) != FFX_OK) Fail("Brixelizer destroy");
    std::puts("oot3d_brixelizer_gi_context_probe: ok");
    return 0;
}
