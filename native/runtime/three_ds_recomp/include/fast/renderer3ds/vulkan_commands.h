#pragma once

#include <vulkan/vulkan.h>

namespace Fast::Renderer3ds {

// Commands newer than Vulkan 1.0 are obtained from the loader at run time, so
// an executable imports only Vulkan 1.0 and WSI entry points from vulkan-1.dll.
// Windows refuses to load a process whose import a loader lacks (0xC0000139,
// issues 12 and 35); obtained this way, an older loader reaches the
// renderer's own error instead.
template <typename Command>
Command VulkanInstanceCommand(VkInstance instance, const char* name) noexcept {
    return reinterpret_cast<Command>(vkGetInstanceProcAddr(instance, name));
}

// The core name first, then the extension alias that provides the same
// command on a device created below that core version.
template <typename Command>
Command VulkanDeviceCommand(VkDevice device, const char* name, const char* alias = nullptr) noexcept {
    auto command = reinterpret_cast<Command>(vkGetDeviceProcAddr(device, name));
    if (command == nullptr && alias != nullptr)
        command = reinterpret_cast<Command>(vkGetDeviceProcAddr(device, alias));
    return command;
}

} // namespace Fast::Renderer3ds
