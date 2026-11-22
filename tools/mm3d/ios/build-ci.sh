#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$repo"
work="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/remasked-ios.XXXXXXXX")"

fetch() {
    local name="$1" url="$2" digest="$3"
    if [[ -n "${REMASKED_IOS_VENDOR_CACHE:-}" && -f "$REMASKED_IOS_VENDOR_CACHE/$name" ]]; then
        cp "$REMASKED_IOS_VENDOR_CACHE/$name" "$work/$name"
    else
        curl --fail --location --retry 3 --silent --show-error "$url" --output "$work/$name"
    fi
    printf '%s  %s\n' "$digest" "$work/$name" | shasum -a 256 --check --status
}

fetch azahar.tar.xz \
    https://github.com/azahar-emu/azahar/releases/download/2127.0-rc4/azahar-unified-source-2127.0-rc4.tar.xz \
    feb86167ed5c1d42e281afc1d180bbd47b2f62eba9fc4b54ec7712487046d768
fetch moltenvk.tar \
    https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-all.tar \
    562a15a29bc358446a56a4091c5f7e08f604184187c1d34f712148b61ef17276

mkdir "$work/upstream"
tar -xf "$work/azahar.tar.xz" --strip-components 1 -C "$work/upstream"
tar -xf "$work/moltenvk.tar" -C "$work"
test "$(cat "$work/upstream/GIT-COMMIT")" = 396b4c3c947afd69fada5c4a1270da0d27174e76
(
    cd "$work/upstream"
    git apply --check "$repo/tools/mm3d/ios/azahar-ios.patch"
    git apply "$repo/tools/mm3d/ios/azahar-ios.patch"
)
ln -s "$repo/src/mm3d" "$work/upstream/src/mm3d"

prefix_flags="-ffile-prefix-map=\"$work\"=. -ffile-prefix-map=\"$repo\"=. -ffile-prefix-map=\"$HOME\"=build-home"
cmake -S "$work/upstream" -B "$work/core" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
    "-DCMAKE_C_FLAGS=-DIOS $prefix_flags" "-DCMAKE_CXX_FLAGS=-DIOS $prefix_flags" \
    "-DCMAKE_OBJC_FLAGS=$prefix_flags" "-DCMAKE_OBJCXX_FLAGS=$prefix_flags" \
    -DCITRA_USE_PRECOMPILED_HEADERS=OFF -DENABLE_LTO=OFF -DENABLE_BUILTIN_KEYBLOB=OFF \
    -DENABLE_LIBRETRO=ON -DENABLE_VULKAN=ON -DENABLE_QT=OFF -DENABLE_SDL2=OFF \
    -DENABLE_CUBEB=OFF -DENABLE_OPENAL=OFF -DENABLE_FFMPEG=OFF -DENABLE_LIBUSB=OFF \
    -DENABLE_ROOM=OFF -DENABLE_ROOM_STANDALONE=OFF -DENABLE_WEB_SERVICE=OFF \
    -DENABLE_DISCORD_RPC=OFF -DENABLE_QT_UPDATE_CHECKER=OFF -DENABLE_CTEST=OFF
grep -qx 'ENABLE_BUILTIN_KEYBLOB:BOOL=OFF' "$work/core/CMakeCache.txt"
cmake --build "$work/core" --target azahar_libretro --parallel "${REMASKED_IOS_JOBS:-4}"

version="$(python3 tools/mm3d/release_version.py)"
python3 tools/mm3d/ios/package_unsigned.py \
    --core-library "$work/core/bin/Release/azahar_libretro.dylib" \
    --moltenvk "$work/MoltenVK/MoltenVK" \
    --output "artifacts/Majora-ReMasked-$version-iOS-arm64-Unsigned.ipa"
