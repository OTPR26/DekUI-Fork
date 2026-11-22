#!/usr/bin/env bash
set -euo pipefail
python3 -m unittest discover -s tools/mm3d -p 'test_*.py'
python3 tools/mm3d/build_desktop_tp_runtime.py
cmake -S . -B build-deck -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_LINKER=/etc/bin/ld.lld \
  -DENABLE_MM3D_PRESENTATION=ON -DENABLE_MM3D_GAME_FOLDER=ON \
  -DENABLE_BUILTIN_KEYBLOB=OFF -DENABLE_QT=ON -DENABLE_VULKAN=ON \
  -DENABLE_LIBRETRO=OFF -DENABLE_ROOM_STANDALONE=OFF \
  -DENABLE_DISCORD_RPC=OFF -DENABLE_QT_UPDATE_CHECKER=OFF
cmake --build build-deck --parallel 4
ctest --test-dir build-deck --output-on-failure
df -h .
du -sh build-deck .ccache externals
cmake --build build-deck --target bundle --parallel 4
mapfile -t images < <(find build-deck/bundle -maxdepth 1 -name '*.AppImage')
test "${#images[@]}" -eq 1
release_version=$(python3 tools/mm3d/release_version.py)
python3 tools/mm3d/package_deck.py "${images[0]}" "artifacts/Majora-ReMasked-${release_version}-Linux-x86_64.tar.gz"
df -h .

python3 tools/mm3d/linux/smoke_setup.py "artifacts/Majora-ReMasked-${release_version}-Linux-x86_64.tar.gz"
