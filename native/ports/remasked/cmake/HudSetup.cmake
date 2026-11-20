# SPDX-License-Identifier: GPL-3.0-or-later
function(remasked_link_hud_setup app root target)
  find_program(REMASKED_RUSTUP rustup REQUIRED)
  execute_process(COMMAND "${REMASKED_RUSTUP}" which --toolchain stable rustc
    OUTPUT_VARIABLE rustc OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  set(forge "${root}/tools/triaevum_forge")
  set(output "${CMAKE_CURRENT_BINARY_DIR}/hud-setup")
  set(archive "${output}/${target}/release/libremasked_hud.a")
  file(GLOB_RECURSE sources CONFIGURE_DEPENDS
    "${forge}/crates/remasked-hud/src/*.rs"
    "${forge}/crates/forge-assets/src/*.rs"
    "${forge}/crates/forge-base/src/*.rs")
  string(ASCII 31 rustflag_separator)
  if(ANDROID)
    get_filename_component(ndk_bin "${CMAKE_C_COMPILER}" DIRECTORY)
    string(REGEX REPLACE "^android-" "" ndk_api "${ANDROID_PLATFORM}")
    set(target_env
      "CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER=${ndk_bin}/aarch64-linux-android${ndk_api}-clang")
  endif()
  add_custom_command(OUTPUT "${archive}"
    COMMAND "${CMAKE_COMMAND}" -E env
      "RUSTC=${rustc}" ${target_env}
      "CARGO_ENCODED_RUSTFLAGS=--remap-path-prefix=${root}=.${rustflag_separator}--remap-path-prefix=${output}=hud-setup"
      "${REMASKED_RUSTUP}" run stable cargo build --locked --release
      --manifest-path "${forge}/Cargo.toml" --package remasked-hud --target "${target}"
      --target-dir "${output}"
    DEPENDS ${sources} "${forge}/Cargo.toml" "${forge}/Cargo.lock"
      "${forge}/crates/remasked-hud/Cargo.toml"
      "${root}/titles/mm3d/dekui/setup/setup-fingerprints.json"
    VERBATIM)
  add_custom_target(remasked_hud_setup DEPENDS "${archive}")
  add_dependencies("${app}" remasked_hud_setup)
  target_link_libraries("${app}" PRIVATE "${archive}")
endfunction()
