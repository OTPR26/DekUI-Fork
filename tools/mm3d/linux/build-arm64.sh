#!/usr/bin/env bash
set -euo pipefail
test "$(uname -m)" = aarch64
python3 -m unittest discover -s tools/mm3d -p 'test_*.py'
cmake -S . -B build-linux-arm64 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DUSE_SYSTEM_QT=ON -DUSE_SYSTEM_SDL2=ON -DENABLE_LTO=OFF \
  -DENABLE_MM3D_PRESENTATION=ON -DENABLE_MM3D_GAME_FOLDER=ON \
  -DENABLE_BUILTIN_KEYBLOB=OFF -DENABLE_QT=ON -DENABLE_VULKAN=ON \
  -DENABLE_LIBRETRO=OFF -DENABLE_ROOM_STANDALONE=OFF \
  -DENABLE_DISCORD_RPC=OFF -DENABLE_QT_UPDATE_CHECKER=OFF
cmake --build build-linux-arm64 --parallel 4
ctest --test-dir build-linux-arm64 --output-on-failure
cmake --build build-linux-arm64 --target bundle --parallel 4
mapfile -t images < <(find build-linux-arm64/bundle -maxdepth 1 -name '*.AppImage')
test "${#images[@]}" -eq 1
python3 tools/mm3d/build_desktop_tp_runtime.py
release_version=$(python3 tools/mm3d/release_version.py)
package="artifacts/DekUI-Fork-${release_version}-Linux-arm64.tar.gz"
python3 tools/mm3d/package_deck.py "${images[0]}" "$package"
python3 tools/mm3d/linux/smoke_setup.py "$package"
python3 tools/mm3d/check_binary_arch.py "$package" arm64
python3 tools/mm3d/package_source.py "artifacts/DekUI-Fork-${release_version}-Linux-arm64-Source.tar.gz"
ccache -s
