# Settings shared by the project's own targets, applied in the order the per-target blocks used to:
# the warning override (unless NO_WARNING_OVERRIDE), NOMINMAX and the static MSVC runtime on Windows,
# and the IDE folder.
#   oot3d_target_defaults(<target> [NO_WARNING_OVERRIDE] [FOLDER <folder>])
function(oot3d_target_defaults target)
    cmake_parse_arguments(ARG "NO_WARNING_OVERRIDE" "FOLDER" "" ${ARGN})
    if(NOT ARG_NO_WARNING_OVERRIDE)
        target_compile_options(${target} PRIVATE "${WARNING_OVERRIDE}")
    endif()
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_compile_definitions(${target} PRIVATE NOMINMAX)
        set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    if(ARG_FOLDER)
        set_property(TARGET ${target} PROPERTY FOLDER ${ARG_FOLDER})
    endif()
endfunction()
