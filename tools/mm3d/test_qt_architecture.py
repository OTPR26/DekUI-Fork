"""Check the Qt installer contract without downloading an SDK."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

class QtArchitectureTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cmake'),'CMake is required for toolchain configuration tests')
    def test_windows_native_target_and_host_tools(self):
        module=(Path(__file__).resolve().parents[2]/'CMakeModules/DownloadExternals.cmake').as_posix()
        for cpu,expected,folder in [('arm64','win64_msvc2022_arm64_cross_compiled','msvc2022_arm64'),('x86_64','win64_msvc2022_64','msvc2022_64')]:
            with self.subTest(cpu=cpu),tempfile.TemporaryDirectory() as tmp:
                script=Path(tmp)/'check.cmake';script.write_text(f'''include("{module}")
set(WIN32 TRUE)
set(MINGW FALSE)
set(MSVC TRUE)
set(ARCHITECTURE {cpu})
set(CMAKE_HOST_SYSTEM_PROCESSOR ARM64)
determine_qt_parameters(6.10.3 host type arch folder host_type host_arch host_folder)
if(NOT arch STREQUAL "{expected}" OR NOT folder STREQUAL "{folder}")
 message(FATAL_ERROR "Wrong target Qt architecture: ${{arch}} / ${{folder}}")
endif()
if(NOT host_arch STREQUAL "win64_msvc2022_64" OR NOT host_folder STREQUAL "msvc2022_64")
 message(FATAL_ERROR "Missing compatible Qt host tools")
endif()
''')
                subprocess.run(['cmake','-P',str(script)],check=True,capture_output=True,text=True)

if __name__=='__main__':unittest.main()
