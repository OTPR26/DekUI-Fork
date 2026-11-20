# An explicit private source directory is required; never download game code.
function(remasked_add_static_title root directory)
  file(READ "${directory}/TITLE_SOURCE_MANIFEST.json" metadata)
  string(JSON title GET "${metadata}" title)
  string(JSON code GET "${metadata}" code_sha256)
  if(NOT title STREQUAL "mm3d" OR NOT code STREQUAL
      "6eec27b1827eab2c87744ac38066d569ffe6fe75790b1eb3300bd91f5099038d")
    message(FATAL_ERROR "Static title must match the pinned MM3D code inventory")
  endif()
  include("${root}/tools/triaevum_release/cmake/TranslatedTitleSources.cmake")
  triaevum_translated_title_sources("${directory}" sources)
  include("${root}/tools/triaevum_release/cmake/TitleSupport.cmake")
  if(NOT TARGET triaevum_title_whole_aot_support)
    triaevum_add_title_support("${root}" "${root}/tools/oot3d/native_a32_runtime/upstream")
  endif()
  add_library(remasked_static_title STATIC ${sources}
    "${root}/tools/oot3d/native_game_runtime/triaevum_title_whole_aot_plugin.cpp")
  # The dynamic title and host normally have independent symbol tables.
  # Static mobile linkage needs distinct names for the two dispatch layers.
  target_compile_definitions(remasked_static_title PRIVATE
    OOT3D_NATIVE_GENERATED_WHOLE_AOT=1
    Oot3dWholeAotEntryPoints=ReMaskedTitleEntryPoints
    ExecuteOot3dWholeAotFunction=ReMaskedTitleExecute)
  target_compile_options(remasked_static_title PRIVATE -O1 -g0 -ffp-model=strict
    "-ffile-prefix-map=${root}=." "-ffile-prefix-map=${directory}=title")
  target_include_directories(remasked_static_title PRIVATE "${directory}")
  target_link_libraries(remasked_static_title PUBLIC triaevum_title_whole_aot_support)
endfunction()
