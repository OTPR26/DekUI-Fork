#include <FidelityFX/host/backends/vk/ffx_vk.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// FidelityFX 1.1.4 wires the optional frame-generation swapchain callback
// into every Vulkan backend interface, even when only SSSR is built. Keep the
// focused renderer module independent from the much larger frame-interpolation
// swapchain implementation while returning the SDK's documented unsupported
// result if an unrelated caller ever tries to invoke it.
extern "C" FFX_API FfxErrorCode
ffxSetFrameGenerationConfigToSwapchainVK(
    const FfxFrameGenerationConfig*) {
    return FFX_ERROR_INVALID_ARGUMENT;
}

#ifdef _WIN32
// The backend calls two Vulkan 1.1 commands directly. On Windows this target's
// compile definitions route those calls here, where they are taken from the
// loaded vulkan-1.dll at first use: the runtime then imports only Vulkan 1.0
// and WSI commands, and still starts with an older loader (issues 12 and 35).
// Without the command the query leaves its output as the caller initialized it.
namespace {
template <typename Command>
Command LoaderCommand(const char* name) noexcept {
    const HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
    return loader ? reinterpret_cast<Command>(GetProcAddress(loader, name)) : nullptr;
}
} // namespace

extern "C" void VKAPI_CALL oot3dFfxGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                                                              VkPhysicalDeviceFeatures2* features) {
    static const auto command = LoaderCommand<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2");
    if (command != nullptr)
        command(physicalDevice, features);
}

extern "C" void VKAPI_CALL oot3dFfxGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                                                VkPhysicalDeviceProperties2* properties) {
    static const auto command = LoaderCommand<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2");
    if (command != nullptr)
        command(physicalDevice, properties);
}
#endif
