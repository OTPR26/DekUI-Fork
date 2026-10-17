#!/usr/bin/env bash
set -euo pipefail
export CCACHE_DIR="$PWD/.ccache-windows"
export CCACHE_MAXSIZE=2G
export CCACHE_COMPILERCHECK=content
python -m unittest discover -s tools/mm3d -p 'test_*.py'
cmake -S . -B build-windows -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DUSE_SYSTEM_QT=ON -DUSE_SYSTEM_SDL2=ON -DENABLE_LTO=OFF \
  -DENABLE_MM3D_PRESENTATION=ON -DENABLE_MM3D_GAME_FOLDER=ON \
  -DENABLE_BUILTIN_KEYBLOB=OFF -DENABLE_QT=ON -DENABLE_VULKAN=ON \
  -DENABLE_LIBRETRO=OFF -DENABLE_ROOM_STANDALONE=OFF \
  -DENABLE_DISCORD_RPC=OFF -DENABLE_QT_UPDATE_CHECKER=OFF
cmake --build build-windows --parallel 4
ctest --test-dir build-windows -C Release --output-on-failure
cmake --build build-windows --target bundle --parallel 4
mkdir -p build-windows/bundle/licenses
cp -R /ucrt64/share/licenses/. build-windows/bundle/licenses/
python tools/mm3d/windows_python.py build-windows/python-embed.zip
release_version=$(python tools/mm3d/release_version.py)
package_path="artifacts/Dekui-${release_version}-Windows-x64.zip"
python tools/mm3d/package_windows.py build-windows/bundle "$package_path" --python-archive build-windows/python-embed.zip
python tools/mm3d/windows/smoke_setup.py "$package_path"
ccache -s
