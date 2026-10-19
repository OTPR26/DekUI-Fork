#!/usr/bin/env bash
set -euo pipefail
export CCACHE_DIR="$PWD/.ccache-macos"
export CCACHE_MAXSIZE=2G
export CCACHE_COMPILERCHECK=content
python3 -m unittest discover -s tools/mm3d -p 'test_*.py'
python3 tools/mm3d/build_desktop_tp_runtime.py
cmake -S . -B build-macos -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DUSE_SYSTEM_QT=OFF -DUSE_SYSTEM_SDL2=OFF -DUSE_SYSTEM_MOLTENVK=OFF \
  -DENABLE_LTO=OFF -DENABLE_MM3D_PRESENTATION=ON -DENABLE_MM3D_GAME_FOLDER=ON \
  -DENABLE_BUILTIN_KEYBLOB=OFF -DENABLE_QT=ON -DENABLE_VULKAN=ON \
  -DENABLE_LIBRETRO=OFF -DENABLE_ROOM_STANDALONE=OFF \
  -DENABLE_DISCORD_RPC=OFF -DENABLE_QT_UPDATE_CHECKER=OFF
cmake --build build-macos --parallel 3
ctest --test-dir build-macos -C Release --output-on-failure
cmake --build build-macos --target bundle --parallel 3
python3 tools/mm3d/macos_python.py build-macos/python-runtime.tar.gz
release_version=$(python3 tools/mm3d/release_version.py)
package_path="artifacts/DekUI-Fork-${release_version}-macOS-arm64.zip"
python3 tools/mm3d/package_macos.py build-macos/bundle/azahar.app "$package_path" \
  --python-archive build-macos/python-runtime.tar.gz
python3 tools/mm3d/macos/smoke_setup.py "$package_path"
ccache -s
