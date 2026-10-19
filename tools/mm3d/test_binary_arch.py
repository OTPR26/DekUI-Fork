import struct
import tempfile
import unittest
import zipfile
from pathlib import Path
from check_binary_arch import architecture, check


def pe(machine):
    data = bytearray(256)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 128)
    data[128:132] = b'PE\0\0'
    struct.pack_into('<H', data, 132, machine)
    return data


class PackageArchitectureTests(unittest.TestCase):
    def test_rejects_x64_library_inside_arm64_package(self):
        with tempfile.TemporaryDirectory() as directory:
            package = Path(directory) / 'arm64.zip'
            with zipfile.ZipFile(package, 'w') as archive:
                archive.writestr('dekui/Majora-ReMasked.exe', pe(0xaa64))
                archive.writestr('dekui/plugins/platforms/qwindows.dll', pe(0x8664))
            with self.assertRaisesRegex(ValueError, 'qwindows.dll.*found x64'):
                check(package, 'arm64')

    def test_native_python_extensions_are_checked_too(self):
        with tempfile.TemporaryDirectory() as directory:
            package = Path(directory) / 'arm64.zip'
            with zipfile.ZipFile(package, 'w') as archive:
                archive.writestr('dekui/Majora-ReMasked.exe', pe(0xaa64))
                archive.writestr('dekui/runtime/python/_socket.pyd', pe(0xaa64))
            check(package, 'arm64')

    def test_elf_arm64_and_invalid_payload(self):
        data = bytearray(64)
        data[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<H', data, 18, 183)
        self.assertEqual(architecture(data), 'arm64')
        struct.pack_into('<H', data, 18, 62)
        self.assertEqual(architecture(data), 'x64')
        with self.assertRaises(ValueError):
            architecture(b'not an executable')
