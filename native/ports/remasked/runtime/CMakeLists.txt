cmake_minimum_required(VERSION 3.30)
project(ReMaskedNativeGame LANGUAGES C CXX OBJC OBJCXX)
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/BuildPrivacy.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/PublicMobileTitle.cmake")
if(NOT CMAKE_SYSTEM_NAME STREQUAL "tvOS" AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
  message(FATAL_ERROR "The native game host requires iOS or tvOS")
endif()
set(REMASKED_NATIVE_RUNTIME ON CACHE BOOL "Use the configured Apple mobile SDK without replacing the toolchain" FORCE)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(TRIAEVUM_TITLES "oot3d;mm3d" CACHE STRING "Native prototype title" FORCE)
set(TRIAEVUM_PRODUCT_TITLE mm3d CACHE STRING "Selected native title" FORCE)
set(TRIAEVUM_DEVELOPER_TOOLS OFF CACHE BOOL "" FORCE)
set(OOT3D_WHOLE_AOT_PRODUCT_MODE ON CACHE BOOL "" FORCE)
set(OOT3D_DIRECT_AOT_PLUGIN ON CACHE BOOL "" FORCE)
set(OOT3D_ENABLE_VULKAN_RENDERER ON CACHE BOOL "" FORCE)
set(THREE_DS_RECOMP_ENABLE_NRI ON CACHE BOOL "" FORCE)
foreach(feature NRI_UPSCALER_SDKS NRI_NIS DLSS_MODULE CACAO SSSR IMMERSE OFFER_EXPERIMENTAL_GI)
  set(THREE_DS_RECOMP_ENABLE_${feature} OFF CACHE BOOL "" FORCE)
endforeach()
set(ENABLE_SCRIPTING OFF CACHE BOOL "" FORCE)
set(INCLUDE_MPQ_SUPPORT OFF CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(REMASKED_TITLE_BUILD "" CACHE PATH "Private matching tvOS title build")
remasked_reject_bundled_public_title()
set(remasked_title_link "${REMASKED_TITLE_BUILD}/libremasked_static_title.a")

set(REMASKED_MOLTENVK_ROOT "" CACHE PATH "Verified MoltenVK package")
set(REMASKED_MOLTENVK_SLICE "tvos-arm64_x86_64-simulator" CACHE STRING "Matching XCFramework slice")
set(molten "${REMASKED_MOLTENVK_ROOT}/MoltenVK/static/MoltenVK.xcframework/${REMASKED_MOLTENVK_SLICE}/libMoltenVK.a")
if(NOT EXISTS "${molten}" OR NOT EXISTS "${remasked_title_link}")
  message(FATAL_ERROR "Matching private title and MoltenVK libraries are required")
endif()
set(Vulkan_LIBRARY "${molten}" CACHE FILEPATH "Static MoltenVK" FORCE)
set(Vulkan_INCLUDE_DIR "${REMASKED_MOLTENVK_ROOT}/MoltenVK/include" CACHE PATH "Vulkan headers" FORCE)
get_filename_component(root "../../.." ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
add_subdirectory("${root}" product)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  target_compile_definitions(oot3d_mm3d_single_screen PUBLIC REMASKED_IOS_GAME=1)
  target_compile_definitions(oot3d_native_game PRIVATE REMASKED_IOS_GAME=1)
endif()
include(FetchContent)
FetchContent_GetProperties(SDL2)
FetchContent_GetProperties(ImGui)
FetchContent_GetProperties(nri)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/patch_sdl_motion.py" "${sdl2_SOURCE_DIR}"
  COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/patch_sdl_scenes.py" "${sdl2_SOURCE_DIR}"
  COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/patch_imgui_apple.py" "${imgui_SOURCE_DIR}"
  COMMAND_ERROR_IS_FATAL ANY)
target_compile_definitions(ImGui PRIVATE REMASKED_APPLE_GAME=1)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/patch_nri_static_vulkan.py" "${nri_SOURCE_DIR}"
  COMMAND_ERROR_IS_FATAL ANY)
target_compile_definitions(NRI_VK PRIVATE REMASKED_APPLE_STATIC_VERTEX_STRIDE=1)
target_compile_definitions(oot3d_native_a32_process PRIVATE TRIAEVUM_STATIC_TITLE_AOT=1)
target_compile_definitions(oot3d_native_game PRIVATE REMASKED_APPLE_GAME=1 REMASKED_NATIVE_GAME=1)
target_sources(oot3d_native_game PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/apple_game_host.mm"
  "${CMAKE_CURRENT_SOURCE_DIR}/apple_duo.mm")
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  target_sources(oot3d_native_game PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/duo_layout_observer.mm")
endif()
target_link_libraries(oot3d_native_game PRIVATE "${remasked_title_link}")
target_compile_definitions(NRI_Shared PRIVATE REMASKED_STATIC_MOLTENVK=1)
target_compile_definitions(NRI PRIVATE REMASKED_STATIC_MOLTENVK=1)
target_link_libraries(NRI PUBLIC "${molten}" "-framework Metal" "-framework Foundation" "-framework QuartzCore" "-framework IOSurface")
set_target_properties(oot3d_native_game PROPERTIES MACOSX_BUNDLE TRUE
  OUTPUT_NAME ReMaskedNativeGame
  MACOSX_BUNDLE_GUI_IDENTIFIER io.github.otpr26.remasked.nativeprototype.game.tvos
  MACOSX_BUNDLE_BUNDLE_NAME "ReMasked Native Game"
  XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(REMASKED_APPLE_PLATFORM iPhoneOS)
  set(REMASKED_APPLE_IDENTIFIER io.github.otpr26.remasked.nativeprototype.game.ios)
  set(REMASKED_APPLE_DISPLAY_NAME "Majora ReMasked")
  set(REMASKED_APPLE_DEVICE_FAMILY "<integer>1</integer><integer>2</integer>")
  set(REMASKED_APPLE_ORIENTATIONS "<key>UISupportedInterfaceOrientations</key><array><string>UIInterfaceOrientationPortrait</string><string>UIInterfaceOrientationLandscapeLeft</string><string>UIInterfaceOrientationLandscapeRight</string></array>")
  if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(REMASKED_APPLE_PLATFORM iPhoneSimulator)
  endif()
else()
  set(REMASKED_APPLE_PLATFORM AppleTVOS)
  set(REMASKED_APPLE_IDENTIFIER io.github.otpr26.remasked.nativeprototype.game.tvos)
  set(REMASKED_APPLE_DISPLAY_NAME "Majora's Mask 3D")
  set(REMASKED_APPLE_DEVICE_FAMILY "<integer>3</integer>")
  set(REMASKED_APPLE_ORIENTATIONS "")
  if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(REMASKED_APPLE_PLATFORM AppleTVSimulator)
  endif()
endif()
set_target_properties(oot3d_native_game PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER "${REMASKED_APPLE_IDENTIFIER}")
# The executable is defined in the product subdirectory; resolve host variables
# here rather than relying on that subdirectory's configure-time scope.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
  "${CMAKE_CURRENT_BINARY_DIR}/ReMaskedInfo.plist")
set_target_properties(oot3d_native_game PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/ReMaskedInfo.plist")



# Private owner assets are copied into the signed, immutable bundle, never caches.
set(REMASKED_OWNER_CONTENT "" CACHE PATH "Private verified owner resources; never commit or publish")
if(REMASKED_OWNER_CONTENT)
  if(NOT EXISTS "${REMASKED_OWNER_CONTENT}/owner-content.json")
    message(FATAL_ERROR "Owner resources require a verified content manifest")
  endif()
  add_custom_target(remasked_owner_content
    COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different "${REMASKED_OWNER_CONTENT}"
      "$<TARGET_BUNDLE_DIR:oot3d_native_game>/OwnerContent"
    VERBATIM)
  add_dependencies(oot3d_native_game remasked_owner_content)
endif()

# Cloud access stays disabled until an authorized signed container is configured.
set(REMASKED_ICLOUD_CONTAINER "" CACHE STRING "Explicit CloudKit container authorized by the signing profile")
target_sources(oot3d_native_game PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/save_sync.mm")
# The app target belongs to the product subdirectory. A source property set
# only in this wrapper's directory does not reach its compile rule. ARC must
# retain async CloudKit state across drained startup autorelease pools.
target_compile_options(oot3d_native_game PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
target_compile_definitions(oot3d_native_game PRIVATE REMASKED_ICLOUD_CONTAINER="${REMASKED_ICLOUD_CONTAINER}")
target_include_directories(oot3d_native_game PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(oot3d_native_game PRIVATE "-framework CloudKit" "-framework Security")

target_sources(oot3d_native_game PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/save_sync_panel.cpp")
# These host-service tests also use the static title ABI in a mobile build.
target_link_libraries(oot3d_native_a32_ctr_host_tests PRIVATE "${remasked_title_link}")

set(REMASKED_GAME_CONTENT "" CACHE PATH "Private local game inputs for owner-device testing only")
if(REMASKED_GAME_CONTENT)
  if(NOT EXISTS "${REMASKED_GAME_CONTENT}/PrivateGame/process-manifest.json")
    message(FATAL_ERROR "Private game input manifest is required")
  endif()
  file(SHA256 "${REMASKED_GAME_CONTENT}/PrivateGame/code.bin" game_code_hash)
  if(NOT game_code_hash STREQUAL "6eec27b1827eab2c87744ac38066d569ffe6fe75790b1eb3300bd91f5099038d")
    message(FATAL_ERROR "Private game code does not match this native title")
  endif()
  add_custom_target(remasked_game_content
    COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different "${REMASKED_GAME_CONTENT}"
      "$<TARGET_BUNDLE_DIR:oot3d_native_game>/GameContent"
    VERBATIM)
  add_dependencies(oot3d_native_game remasked_game_content)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set_property(SOURCE "${root}/runtime/three_ds_recomp/src/fast/backends/gfx_sdl2.cpp"
    TARGET_DIRECTORY three_ds_recomp_runtime APPEND PROPERTY COMPILE_DEFINITIONS REMASKED_IOS_NATIVE_DISPLAY=1)
  target_sources(oot3d_native_game PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ios/touch_controls.mm" "${CMAKE_CURRENT_SOURCE_DIR}/ios/ios_host.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/ios/drive_data.mm" "${CMAKE_CURRENT_SOURCE_DIR}/ios/rom_setup.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ios/hud_download.mm")
  include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HudSetup.cmake")
  if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    remasked_link_hud_setup(oot3d_native_game "${root}" aarch64-apple-ios-sim)
  else()
    remasked_link_hud_setup(oot3d_native_game "${root}" aarch64-apple-ios)
  endif()
  # First-run installs have no bundled title data. Keep the controller
  # database in the resource directory that the prepared launch profile mounts.
  set(remasked_controller_db "${root}/runtime/triaevum_resources/gamecontrollerdb.txt")
  set_property(SOURCE "${remasked_controller_db}" TARGET_DIRECTORY oot3d_native_game
    PROPERTY MACOSX_PACKAGE_LOCATION "Resources/GameContent/resources")
  set(remasked_presentation_defaults "${CMAKE_CURRENT_SOURCE_DIR}/../resources/presentation.json")
  set_property(SOURCE "${remasked_presentation_defaults}" TARGET_DIRECTORY oot3d_native_game
    PROPERTY MACOSX_PACKAGE_LOCATION "Resources")
  target_sources(oot3d_native_game PRIVATE "${remasked_controller_db}" "${remasked_presentation_defaults}")
  target_link_libraries(oot3d_native_game PRIVATE "-framework GameController" "-framework UniformTypeIdentifiers")
  set(remasked_fonts "${CMAKE_CURRENT_BINARY_DIR}/remasked_interface_font_data.h")
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_interface_fonts.py"
    --fonts "${CMAKE_CURRENT_SOURCE_DIR}/../resources/fonts" --output "${remasked_fonts}" COMMAND_ERROR_IS_FATAL ANY)
  set_property(SOURCE "${root}/runtime/three_ds_recomp/src/ship/window/gui/Gui.cpp"
    TARGET_DIRECTORY three_ds_recomp_runtime APPEND PROPERTY COMPILE_DEFINITIONS
    REMASKED_ANDROID_MENU=1 "REMASKED_INTERFACE_FONTS_HEADER=\"${remasked_fonts}\"")
endif()
