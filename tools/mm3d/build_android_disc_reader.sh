#!/usr/bin/env bash
set -euo pipefail
source_dir="$RUNNER_TEMP/tp-disc-reader"
mkdir -p "$source_dir"
curl --fail --location --silent --show-error \
  https://codeload.github.com/encounter/nod/tar.gz/ebd80cac99b48a323200d84365e07a58cc27412d \
  | tar -xz --strip-components=1 -C "$source_dir"
toolchain="$ANDROID_HOME/ndk/27.3.13750724/toolchains/llvm/prebuilt/linux-x86_64/bin"
export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$toolchain/aarch64-linux-android24-clang"
export CC_aarch64_linux_android="$toolchain/aarch64-linux-android24-clang"
export CXX_aarch64_linux_android="$toolchain/aarch64-linux-android24-clang++"
export AR_aarch64_linux_android="$toolchain/llvm-ar"
export RUSTFLAGS="--remap-path-prefix=$RUNNER_TEMP=/build --remap-path-prefix=$HOME=/build"
export CFLAGS_aarch64_linux_android="-ffile-prefix-map=$RUNNER_TEMP=/build -ffile-prefix-map=$HOME=/build"
export CXXFLAGS_aarch64_linux_android="$CFLAGS_aarch64_linux_android"
cargo build --manifest-path "$source_dir/Cargo.toml" --locked --release \
  --target aarch64-linux-android -p nod-ffi --no-default-features \
  --features compress-bzip2-vendored,compress-lzma-vendored,compress-zlib-vendored,compress-zstd-vendored
destination=src/android/app/build/dekuiJniLibs/arm64-v8a
mkdir -p "$destination"
cp "$source_dir/target/aarch64-linux-android/release/libnod.so" "$destination/"
cp "$source_dir/LICENSE-MIT" "$source_dir/LICENSE-APACHE" src/android/app/src/main/assets/
