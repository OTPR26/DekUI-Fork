"""Vulkan imports of Windows binaries stay within what every loader exports."""
from __future__ import annotations

import struct
import tempfile
import unittest
from pathlib import Path

from vulkan_imports import VULKAN_LOADER_BASELINE, eager_imports, vulkan_import_errors


def pe_import_fixture(imports: dict[str, list[str]]) -> bytes:
    # Minimal PE32+ import table, never executed or loaded as code.
    data = bytearray(16384)
    struct.pack_into("<H", data, 0, 0x5A4D)
    struct.pack_into("<I", data, 0x3C, 0x80)
    struct.pack_into("<I", data, 0x80, 0x4550)
    struct.pack_into("<HH", data, 0x84, 0x8664, 0)
    struct.pack_into("<H", data, 0x94, 240)
    struct.pack_into("<H", data, 0x98, 0x20B)
    struct.pack_into("<I", data, 0x98 + 60, len(data))
    struct.pack_into("<I", data, 0x98 + 120, 0x200)
    # Descriptors from 0x200, DLL names from 0x280, thunks from 0x400, hint/name
    # entries from 0x1000; every RVA is its file offset.
    names, thunks = 0x280, 0x400
    strings = 0x1000
    for index, (dll, symbols) in enumerate(imports.items()):
        encoded = dll.encode("ascii") + b"\0"
        data[names:names + len(encoded)] = encoded
        struct.pack_into("<IIIII", data, 0x200 + index * 20, thunks, 0, 0, names, thunks)
        names += len(encoded)
        for symbol in symbols:
            struct.pack_into("<Q", data, thunks, strings)
            encoded = symbol.encode("ascii") + b"\0"
            data[strings + 2:strings + 2 + len(encoded)] = encoded
            strings += 2 + len(encoded)
            thunks += 8
        thunks += 8
    return bytes(data)


class VulkanImportTests(unittest.TestCase):
    def check(self, image: bytes, **options) -> list[str]:
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "TriAevum.exe"
            path.write_bytes(image)
            return vulkan_import_errors(path, **options)

    def test_reads_every_import_directory_entry(self):
        image = pe_import_fixture({"KERNEL32.dll": ["Sleep"], "vulkan-1.dll": ["vkCreateInstance", "vkQueuePresentKHR"]})
        self.assertEqual(eager_imports(image), {"KERNEL32.dll": ("Sleep",),
                                                "vulkan-1.dll": ("vkCreateInstance", "vkQueuePresentKHR")})

    def test_vulkan_1_0_and_wsi_commands_pass(self):
        self.assertEqual(self.check(pe_import_fixture({"vulkan-1.dll": sorted(VULKAN_LOADER_BASELINE)})), [])

    def test_names_every_command_an_older_loader_lacks(self):
        errors = self.check(pe_import_fixture({
            "KERNEL32.dll": ["Sleep"],
            "vulkan-1.dll": ["vkCreateInstance", "vkWaitSemaphores", "vkGetPhysicalDeviceFeatures2"],
        }))
        self.assertEqual(errors, ["TriAevum.exe imports Vulkan commands that older loaders lack: "
                                  "vkGetPhysicalDeviceFeatures2, vkWaitSemaphores"])

    def test_the_device_group_swapchain_commands_are_not_baseline(self):
        self.assertNotIn("vkAcquireNextImage2KHR", VULKAN_LOADER_BASELINE)
        self.assertIn("vkAcquireNextImageKHR", VULKAN_LOADER_BASELINE)

    def test_other_libraries_are_not_restricted(self):
        self.assertEqual(self.check(pe_import_fixture({"d3d12.dll": ["D3D12CreateDevice"]})), [])

    def test_a_file_that_is_not_a_pe_image(self):
        self.assertEqual(self.check(b"MZ generic runtime"), ["TriAevum.exe is not a PE image"])
        self.assertEqual(self.check(b"MZ generic runtime", require_pe=False), [])


if __name__ == "__main__":
    unittest.main()
