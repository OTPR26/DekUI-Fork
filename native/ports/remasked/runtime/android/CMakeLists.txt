cmake_minimum_required(VERSION 3.30)
project(ReMaskedAndroidGame LANGUAGES C CXX)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/BuildPrivacy.cmake")
set(TRIAEVUM_TITLES "oot3d;mm3d" CACHE STRING "" FORCE)
set(TRIAEVUM_PRODUCT_TITLE mm3d CACHE STRING "" FORCE)
set(TRIAEVUM_DEVELOPER_TOOLS OFF CACHE BOOL "" FORCE)
set(REMASKED_TITLE_BUILD "" CACHE PATH "Private matching Android static title")
if(NOT EXISTS "${REMASKED_TITLE_BUILD}/libremasked_static_title.a")
  message(FATAL_ERROR "A matching private Android MM3D static title is required")
endif()
set(THREE_DS_RECOMP_ENABLE_NRI_NIS OFF CACHE BOOL "" FORCE)
set(THREE_DS_RECOMP_ENABLE_NRI_UPSCALER_SDKS OFF CACHE BOOL "" FORCE)
set(THREE_DS_RECOMP_ENABLE_IMMERSE OFF CACHE BOOL "" FORCE)
set(INCLUDE_MPQ_SUPPORT OFF CACHE BOOL "" FORCE)
get_filename_component(root "../../../.." ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(REMASKED_ANDROID_DRIVER_CMAKE "${CMAKE_CURRENT_SOURCE_DIR}/driver/graphics-driver.cmake" CACHE FILEPATH "Isolated driver support" FORCE)
add_subdirectory("${root}/ports/android/runtime" runtime)
target_compile_definitions(oot3d_native_a32_process PRIVATE TRIAEVUM_STATIC_TITLE_AOT=1)
target_compile_definitions(oot3d_native_game PRIVATE REMASKED_NATIVE_GAME=1)
target_sources(oot3d_native_game PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/android_game_host.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/startup_settings.cpp")
target_include_directories(oot3d_native_game PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
# The Android runtime also links the donor's bootstrap DSO, whose empty
# query must not satisfy these symbols before the real static archive is read.
target_link_libraries(oot3d_native_game PRIVATE -Wl,--whole-archive
  "${REMASKED_TITLE_BUILD}/libremasked_static_title.a" -Wl,--no-whole-archive)
target_link_options(oot3d_native_game PRIVATE -Wl,-Bsymbolic-functions)
target_link_options(oot3d_native_game PRIVATE -Wl,-z,max-page-size=16384)



# ReTuned's licensed font faces are embedded before ImGui builds its GPU atlas.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(remasked_fonts "${CMAKE_CURRENT_BINARY_DIR}/remasked_interface_font_data.h")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/generate_interface_fonts.py"
  --fonts "${CMAKE_CURRENT_SOURCE_DIR}/../../resources/fonts" --output "${remasked_fonts}"
  COMMAND_ERROR_IS_FATAL ANY)
set_property(SOURCE "${root}/runtime/three_ds_recomp/src/ship/window/gui/Gui.cpp"
  TARGET_DIRECTORY three_ds_recomp_runtime APPEND PROPERTY COMPILE_DEFINITIONS
  REMASKED_ANDROID_MENU=1 "REMASKED_INTERFACE_FONTS_HEADER=\"${remasked_fonts}\"")

target_include_directories(oot3d_native_game PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# Android secondary display surfaces use ANativeWindow ownership.
target_link_libraries(oot3d_native_game PRIVATE android)
