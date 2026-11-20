#pragma once
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

void* ReMaskedVulkanSymbol(const char* name);
