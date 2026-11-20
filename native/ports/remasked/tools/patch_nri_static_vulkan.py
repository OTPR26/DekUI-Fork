#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Allow pinned NRI to resolve its Vulkan entry point from linked MoltenVK."""
import sys
from pathlib import Path
path = Path(sys.argv[1]) / 'Source/Shared/SharedLibrary.hpp'
source = path.read_text()
marker = '// ReMasked static MoltenVK loader'
if marker not in source:
    anchor = '#else\n#    include <dlfcn.h>'
    if source.count(anchor) != 1:
        raise SystemExit('Pinned NRI shared loader shape changed')
    source = source.replace(anchor, '''#elif defined(REMASKED_STATIC_MOLTENVK)
// ReMasked static MoltenVK loader: no external library or downloaded code.
#    include <vulkan/vulkan_core.h>
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char*);
namespace { char staticVulkanToken; }
Library* nri::LoadSharedLibrary(const char* path) {
    return path && std::strcmp(path, NRI_VULKAN_LOADER_NAME) == 0
        ? reinterpret_cast<Library*>(&staticVulkanToken) : nullptr;
}
void* nri::GetSharedLibraryFunction(Library& library, const char* name) {
    return reinterpret_cast<void*>(&library) == &staticVulkanToken &&
           std::strcmp(name, "vkGetInstanceProcAddr") == 0
        ? reinterpret_cast<void*>(&vkGetInstanceProcAddr) : nullptr;
}
void nri::UnloadSharedLibrary(Library&) {}
#else
#    include <dlfcn.h>''')
    path.write_text(source)

# Portability enumeration belongs to the loader and is not advertised by
# every direct MoltenVK mobile ICD. Never request an absent extension.
root = Path(sys.argv[1])
creation = root / 'Source/Creation/Creation.cpp'
source = creation.read_text()
if '// ReMasked direct ICD extension check' not in source:
    anchor = '    vkResult = vkCreateInstance(&instanceCreateInfo, nullptr, &instance);'
    if source.count(anchor) != 1:
        raise SystemExit('Pinned NRI adapter enumeration changed')
    source = source.replace(anchor, '''    // ReMasked direct ICD extension check
#if defined(REMASKED_STATIC_MOLTENVK)
    {
        auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
            vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
        uint32_t count = 0;
        bool portability = false;
        if (enumerate && enumerate(nullptr, &count, nullptr) == VK_SUCCESS) {
            std::vector<VkExtensionProperties> extensions(count);
            if (enumerate(nullptr, &count, extensions.data()) == VK_SUCCESS)
                for (const auto& extension : extensions)
                    portability |= std::strcmp(extension.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0;
        }
        instanceCreateInfo.enabledExtensionCount = portability ? 2 : 1;
        instanceCreateInfo.flags = portability ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0;
    }
#endif
''' + anchor)
    creation.write_text(source)
device = root / 'Source/VK/DeviceVK.hpp'
source = device.read_text()
anchor = '    desiredInstanceExts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);'
if '// ReMasked: direct ICDs' not in source:
    if source.count(anchor) != 1:
        raise SystemExit('Pinned NRI device extensions changed')
    source = source.replace(anchor, '''    // ReMasked: direct ICDs may omit the loader's portability extension.
    if (IsExtensionSupported(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME, supportedExts))
        desiredInstanceExts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);''')
    # The replacement still contains the anchor; use the comment as an idempotence marker below.
    if source.count('// ReMasked: direct ICDs') > 1:
        raise SystemExit('Static Vulkan patch was applied more than once')
    source = source.replace('    instanceCreateInfo.flags = (VkInstanceCreateFlags)VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,',
        '''    instanceCreateInfo.flags = IsExtensionSupported(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME, desiredInstanceExts)
        ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0;''')
    device.write_text(source)

source = device.read_text()
marker = '// ReMasked mobile uses the supported static vertex-stride path.'
if marker not in source:
    anchor = '    m_IsSupported.maintenance4 = features13.maintenance4;'
    if source.count(anchor) != 1:
        raise SystemExit('Pinned NRI feature admission changed')
    source = source.replace(anchor, '''#if defined(REMASKED_APPLE_STATIC_VERTEX_STRIDE)
    // ReMasked mobile uses the supported static vertex-stride path.
    // MoltenVK can advertise extended dynamic state on simulator GPUs whose
    // Metal implementation cannot change vertex-buffer strides dynamically.
    ExtendedDynamicStateFeatures.extendedDynamicState = false;
#endif
''' + anchor)
    device.write_text(source)

# Keep cache-import rejection observable on Android. The renderer verifies
# provenance/checksums; the driver can still reject the opaque payload.
cache = root / 'Source/VK/PipelineCacheVK.hpp'
source = cache.read_text()
marker = '// ReMasked Android pipeline-cache import diagnostics'
if marker not in source:
    anchor = '    return size < info.initialDataSize ? Result::OUT_OF_DATE : Result::SUCCESS;'
    if source.count(anchor) != 1:
        raise SystemExit('Pinned NRI pipeline-cache admission changed')
    source = source.replace(anchor, '''    // ReMasked Android pipeline-cache import diagnostics
#if defined(__ANDROID__)
    NRI_REPORT_INFO(&m_Device, "ReMasked pipeline cache import: initial=%zu serialized=%zu", info.initialDataSize, size);
#endif
''' + anchor)
    cache.write_text(source)

# Turnip compacts a valid imported cache (observed: 76215988 -> 76215948).
# The original size heuristic discarded all its entries for a 40-byte shrink.
# On Android, reject a smaller import only if it is no larger than an actual
# empty cache made by this same device. Other platforms keep the NRI policy.
policy = Path(__file__).resolve().parents[1] / 'renderer/pipeline_cache_import.h'
(root / 'Source/VK/ReMaskedPipelineCacheImport.h').write_text(policy.read_text())
source = cache.read_text()
marker = '// ReMasked Android accepts compacted nonempty cache imports'
if marker not in source:
    anchor = '    return size < info.initialDataSize ? Result::OUT_OF_DATE : Result::SUCCESS;'
    if source.count(anchor) != 1:
        raise SystemExit('Pinned NRI pipeline-cache size policy changed')
    source = '#if defined(__ANDROID__)\n#include "ReMaskedPipelineCacheImport.h"\n#endif\n' + source
    source = source.replace(anchor, '''    // ReMasked Android accepts compacted nonempty cache imports
    bool usable = size >= info.initialDataSize;
#if defined(__ANDROID__)
    if (!usable) {
        VkPipelineCacheCreateInfo emptyInfo = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        VkPipelineCache empty = VK_NULL_HANDLE;
        if (vk.CreatePipelineCache(m_Device, &emptyInfo, m_Device.GetVkAllocationCallbacks(), &empty) == VK_SUCCESS) {
            size_t emptySize = 0;
            const VkResult emptyResult = vk.GetPipelineCacheData(m_Device, empty, &emptySize, nullptr);
            vk.DestroyPipelineCache(m_Device, empty, m_Device.GetVkAllocationCallbacks());
            if (emptyResult == VK_SUCCESS)
                usable = ReMaskedRenderer::PipelineCacheImportIsUsable(info.initialDataSize, size, emptySize);
            NRI_REPORT_INFO(&m_Device, "ReMasked pipeline cache compaction: initial=%zu serialized=%zu empty=%zu usable=%u",
                info.initialDataSize, size, emptySize, unsigned(usable));
        }
    }
#endif
    return usable ? Result::SUCCESS : Result::OUT_OF_DATE;''')
    cache.write_text(source)
