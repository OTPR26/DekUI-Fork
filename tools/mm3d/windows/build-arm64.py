"""Build and test native Windows ARM64 using the MSVC ARM64 environment."""
import os
from pathlib import Path
import platform
import shutil
import subprocess


def run(*args):
    subprocess.run(args, check=True)


if platform.machine().lower() not in ('arm64', 'aarch64'):
    raise RuntimeError('Run this build on a native Windows ARM64 runner')
build = Path('build-windows-arm64')
run('python', '-m', 'unittest', 'discover', '-s', 'tools/mm3d', '-p', 'test_*.py')
run('cmake', '-S', '.', '-B', str(build), '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl',
    '-DUSE_SYSTEM_QT=OFF', '-DUSE_SYSTEM_SDL2=OFF', '-DENABLE_LTO=OFF',
    '-DENABLE_MM3D_PRESENTATION=ON', '-DENABLE_MM3D_GAME_FOLDER=ON',
    '-DENABLE_BUILTIN_KEYBLOB=OFF', '-DENABLE_QT=ON', '-DENABLE_VULKAN=ON',
    '-DENABLE_LIBRETRO=OFF', '-DENABLE_ROOM_STANDALONE=OFF',
    '-DENABLE_DISCORD_RPC=OFF', '-DENABLE_QT_UPDATE_CHECKER=OFF')
run('cmake', '--build', str(build), '--parallel', '4')
run('ctest', '--test-dir', str(build), '-C', 'Release', '--output-on-failure')
run('cmake', '--build', str(build), '--target', 'bundle', '--parallel', '4')
# Qt's shared libraries use the Microsoft runtime even when the app is static.
redist = Path(os.environ['VCToolsRedistDir']) / 'arm64'
runtime_dirs = list(redist.glob('Microsoft.VC*.CRT'))
if not runtime_dirs:
    raise RuntimeError('Native ARM64 Microsoft C runtime is missing')
for directory in runtime_dirs:
    for dll in directory.glob('*.dll'):
        shutil.copy2(dll, build / 'bundle' / dll.name)
runtime = build / 'python-embed-arm64.zip'
run('python', 'tools/mm3d/windows_python.py', str(runtime), '--architecture', 'arm64')
run('python', 'tools/mm3d/build_desktop_tp_runtime.py')
version = subprocess.check_output(['python', 'tools/mm3d/release_version.py'], text=True).strip()
package = f'artifacts/Majora-ReMasked-{version}-Windows-arm64.zip'
run('python', 'tools/mm3d/package_windows.py', str(build / 'bundle'), package,
    '--python-archive', str(runtime), '--architecture', 'arm64')
run('python', 'tools/mm3d/check_binary_arch.py', package, 'arm64')
run('python', 'tools/mm3d/windows/smoke_setup.py', package)
run('python', 'tools/mm3d/package_source.py',
    f'artifacts/Majora-ReMasked-{version}-Windows-arm64-Source.tar.gz')
