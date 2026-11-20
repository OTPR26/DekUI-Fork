# SPDX-License-Identifier: GPL-3.0-or-later
# Map source, dependency-cache and generated-file paths before creating targets.
# File/macro maps cover compiled diagnostics and assertions as well as debug info.
include_guard(GLOBAL)
get_filename_component(_remasked_source "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
get_filename_component(_remasked_workspace "${_remasked_source}/../../.." ABSOLUTE)
set(_remasked_maps)
if(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
  list(APPEND _remasked_maps "$ENV{HOME}=/builder")
endif()
list(APPEND _remasked_maps "${_remasked_workspace}=/workspace"
  "${_remasked_source}=/src" "${CMAKE_BINARY_DIR}=/build")
foreach(_remasked_map IN LISTS _remasked_maps)
  add_compile_options("-ffile-prefix-map=${_remasked_map}"
    "-fmacro-prefix-map=${_remasked_map}")
endforeach()
unset(_remasked_maps)
unset(_remasked_map)
unset(_remasked_source)
unset(_remasked_workspace)
