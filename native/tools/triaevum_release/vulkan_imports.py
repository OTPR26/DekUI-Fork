"""Check that Windows binaries import only Vulkan 1.0 and WSI entry points.

Windows refuses to start a process when an eager import is missing from the DLL
that provides it (0xC0000139, issues 12 and 35). The Vulkan loader on the
user's machine comes with the graphics driver and may predate Vulkan 1.1, so a
shipped executable imports from vulkan-1.dll only what every loader exports:
the Vulkan 1.0 commands and the original surface, swapchain and Win32 surface
commands. Newer commands are obtained from the loader at run time
(runtime/three_ds_recomp/include/fast/renderer3ds/vulkan_commands.h).
"""

from __future__ import annotations

import argparse
import mmap
import struct
from pathlib import Path
from typing import Sequence

VULKAN_LOADER = "vulkan-1.dll"

# Vulkan 1.0, VK_KHR_surface, the original VK_KHR_swapchain commands (without
# the device-group commands that Vulkan 1.1 added to it) and
# VK_KHR_win32_surface, as declared by the Vulkan headers.
VULKAN_LOADER_BASELINE = frozenset((
    # Vulkan 1.0
    "vkCreateInstance", "vkDestroyInstance", "vkEnumeratePhysicalDevices", "vkGetPhysicalDeviceFeatures",
    "vkGetPhysicalDeviceFormatProperties", "vkGetPhysicalDeviceImageFormatProperties",
    "vkGetPhysicalDeviceProperties", "vkGetPhysicalDeviceQueueFamilyProperties",
    "vkGetPhysicalDeviceMemoryProperties", "vkGetInstanceProcAddr", "vkGetDeviceProcAddr", "vkCreateDevice",
    "vkDestroyDevice", "vkEnumerateInstanceExtensionProperties", "vkEnumerateDeviceExtensionProperties",
    "vkEnumerateInstanceLayerProperties", "vkEnumerateDeviceLayerProperties", "vkGetDeviceQueue",
    "vkQueueSubmit", "vkQueueWaitIdle", "vkDeviceWaitIdle", "vkAllocateMemory", "vkFreeMemory", "vkMapMemory",
    "vkUnmapMemory", "vkFlushMappedMemoryRanges", "vkInvalidateMappedMemoryRanges",
    "vkGetDeviceMemoryCommitment", "vkBindBufferMemory", "vkBindImageMemory",
    "vkGetBufferMemoryRequirements", "vkGetImageMemoryRequirements", "vkGetImageSparseMemoryRequirements",
    "vkGetPhysicalDeviceSparseImageFormatProperties", "vkQueueBindSparse", "vkCreateFence", "vkDestroyFence",
    "vkResetFences", "vkGetFenceStatus", "vkWaitForFences", "vkCreateSemaphore", "vkDestroySemaphore",
    "vkCreateEvent", "vkDestroyEvent", "vkGetEventStatus", "vkSetEvent", "vkResetEvent", "vkCreateQueryPool",
    "vkDestroyQueryPool", "vkGetQueryPoolResults", "vkCreateBuffer", "vkDestroyBuffer", "vkCreateBufferView",
    "vkDestroyBufferView", "vkCreateImage", "vkDestroyImage", "vkGetImageSubresourceLayout",
    "vkCreateImageView", "vkDestroyImageView", "vkCreateShaderModule", "vkDestroyShaderModule",
    "vkCreatePipelineCache", "vkDestroyPipelineCache", "vkGetPipelineCacheData", "vkMergePipelineCaches",
    "vkCreateGraphicsPipelines", "vkCreateComputePipelines", "vkDestroyPipeline", "vkCreatePipelineLayout",
    "vkDestroyPipelineLayout", "vkCreateSampler", "vkDestroySampler", "vkCreateDescriptorSetLayout",
    "vkDestroyDescriptorSetLayout", "vkCreateDescriptorPool", "vkDestroyDescriptorPool",
    "vkResetDescriptorPool", "vkAllocateDescriptorSets", "vkFreeDescriptorSets", "vkUpdateDescriptorSets",
    "vkCreateFramebuffer", "vkDestroyFramebuffer", "vkCreateRenderPass", "vkDestroyRenderPass",
    "vkGetRenderAreaGranularity", "vkCreateCommandPool", "vkDestroyCommandPool", "vkResetCommandPool",
    "vkAllocateCommandBuffers", "vkFreeCommandBuffers", "vkBeginCommandBuffer", "vkEndCommandBuffer",
    "vkResetCommandBuffer", "vkCmdBindPipeline", "vkCmdSetViewport", "vkCmdSetScissor", "vkCmdSetLineWidth",
    "vkCmdSetDepthBias", "vkCmdSetBlendConstants", "vkCmdSetDepthBounds", "vkCmdSetStencilCompareMask",
    "vkCmdSetStencilWriteMask", "vkCmdSetStencilReference", "vkCmdBindDescriptorSets",
    "vkCmdBindIndexBuffer", "vkCmdBindVertexBuffers", "vkCmdDraw", "vkCmdDrawIndexed", "vkCmdDrawIndirect",
    "vkCmdDrawIndexedIndirect", "vkCmdDispatch", "vkCmdDispatchIndirect", "vkCmdCopyBuffer",
    "vkCmdCopyImage", "vkCmdBlitImage", "vkCmdCopyBufferToImage", "vkCmdCopyImageToBuffer",
    "vkCmdUpdateBuffer", "vkCmdFillBuffer", "vkCmdClearColorImage", "vkCmdClearDepthStencilImage",
    "vkCmdClearAttachments", "vkCmdResolveImage", "vkCmdSetEvent", "vkCmdResetEvent", "vkCmdWaitEvents",
    "vkCmdPipelineBarrier", "vkCmdBeginQuery", "vkCmdEndQuery", "vkCmdResetQueryPool",
    "vkCmdWriteTimestamp", "vkCmdCopyQueryPoolResults", "vkCmdPushConstants", "vkCmdBeginRenderPass",
    "vkCmdNextSubpass", "vkCmdEndRenderPass", "vkCmdExecuteCommands",
    # VK_KHR_surface
    "vkDestroySurfaceKHR", "vkGetPhysicalDeviceSurfaceSupportKHR", "vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
    "vkGetPhysicalDeviceSurfaceFormatsKHR", "vkGetPhysicalDeviceSurfacePresentModesKHR",
    # VK_KHR_swapchain
    "vkCreateSwapchainKHR", "vkDestroySwapchainKHR", "vkGetSwapchainImagesKHR", "vkAcquireNextImageKHR",
    "vkQueuePresentKHR",
    # VK_KHR_win32_surface
    "vkCreateWin32SurfaceKHR", "vkGetPhysicalDeviceWin32PresentationSupportKHR",
))


def _rva_offset(sections: list[tuple[int, int, int]], headers: int, rva: int) -> int:
    for virtual_address, size, raw_offset in sections:
        if virtual_address <= rva < virtual_address + size:
            return raw_offset + rva - virtual_address
    if rva < headers:
        return rva
    raise ValueError(f"RVA 0x{rva:x} lies outside every section")


def _string(image: bytes | mmap.mmap, offset: int) -> str:
    end = image.find(b"\0", offset)
    if end < 0:
        raise ValueError("unterminated import name")
    return bytes(image[offset:end]).decode("ascii")


def is_pe(image: bytes | mmap.mmap) -> bool:
    if len(image) < 0x40 or bytes(image[:2]) != b"MZ":
        return False
    pe, = struct.unpack_from("<I", image, 0x3C)
    return bytes(image[pe:pe + 4]) == b"PE\0\0"


def eager_imports(image: bytes | mmap.mmap) -> dict[str, tuple[str, ...]]:
    """Map each DLL of the import directory (not the delay-load table) to its imports."""
    if not is_pe(image):
        raise ValueError("not a PE image")
    pe, = struct.unpack_from("<I", image, 0x3C)
    section_count, = struct.unpack_from("<H", image, pe + 6)
    optional_size, = struct.unpack_from("<H", image, pe + 20)
    optional = pe + 24
    magic, = struct.unpack_from("<H", image, optional)
    if magic == 0x20B:
        directories, thunk_format, ordinal_flag = optional + 112, "<Q", 1 << 63
    elif magic == 0x10B:
        directories, thunk_format, ordinal_flag = optional + 96, "<I", 1 << 31
    else:
        raise ValueError("unknown optional header")
    headers, = struct.unpack_from("<I", image, optional + 60)
    sections = []
    for index in range(section_count):
        entry = optional + optional_size + index * 40
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", image, entry + 8)
        sections.append((virtual_address, max(virtual_size, raw_size), raw_offset))
    import_rva, = struct.unpack_from("<I", image, directories + 8)
    result: dict[str, tuple[str, ...]] = {}
    if import_rva == 0:
        return result
    descriptor = _rva_offset(sections, headers, import_rva)
    thunk_size = struct.calcsize(thunk_format)
    while True:
        original, _, _, name_rva, first = struct.unpack_from("<IIIII", image, descriptor)
        if name_rva == 0:
            return result
        dll = _string(image, _rva_offset(sections, headers, name_rva))
        names = []
        thunk = _rva_offset(sections, headers, original or first)
        while True:
            entry, = struct.unpack_from(thunk_format, image, thunk)
            if entry == 0:
                break
            if entry & ordinal_flag:
                names.append(f"#{entry & 0xFFFF}")
            else:
                names.append(_string(image, _rva_offset(sections, headers, entry & 0x7FFFFFFF) + 2))
            thunk += thunk_size
        result[dll] = tuple(names)
        descriptor += 20


def vulkan_import_errors(path: Path, label: str | None = None, *, require_pe: bool = True) -> list[str]:
    """Errors for eager vulkan-1.dll imports outside the loader baseline.

    A file that is not a PE image is an error only with require_pe."""
    label = label or path.name
    try:
        if path.stat().st_size == 0:
            return [f"{label} is not a PE image"] if require_pe else []
        with path.open("rb") as source, mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as image:
            if not is_pe(image):
                return [f"{label} is not a PE image"] if require_pe else []
            imports = eager_imports(image)
    except (OSError, ValueError, struct.error) as exc:
        return [f"cannot read the imports of {label}: {exc}"]
    errors = []
    for dll, names in imports.items():
        if dll.lower() != VULKAN_LOADER:
            continue
        newer = sorted(set(names) - VULKAN_LOADER_BASELINE)
        if newer:
            errors.append(f"{label} imports Vulkan commands that older loaders lack: {', '.join(newer)}")
    return errors


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("binaries", nargs="+", type=Path)
    args = parser.parse_args(argv)
    errors = [error for path in args.binaries for error in vulkan_import_errors(path, str(path))]
    for error in errors:
        print(error)
    if not errors:
        print(f"{len(args.binaries)} binaries import only Vulkan 1.0 and WSI commands")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
