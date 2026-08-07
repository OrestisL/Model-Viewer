# ---------------------------------------------------------------------------
# mv_compile_shaders(<target> <shader> [<shader> ...])
#
# Compiles GLSL sources to SPIR-V and drops the results in
#   $<TARGET_FILE_DIR:target>/shaders/<name>.spv
# so the executable can load them with a path relative to itself.
# ---------------------------------------------------------------------------

find_program(MV_GLSL_COMPILER
    NAMES glslc glslangValidator
    HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin"
    DOC "GLSL -> SPIR-V compiler shipped with the Vulkan SDK")

if(NOT MV_GLSL_COMPILER)
    message(FATAL_ERROR
        "Could not find glslc or glslangValidator. Install the LunarG Vulkan SDK "
        "and make sure VULKAN_SDK is set, or pass -DMV_GLSL_COMPILER=/path/to/glslc")
endif()

get_filename_component(_mv_glsl_name "${MV_GLSL_COMPILER}" NAME_WE)
message(STATUS "Shader compiler: ${MV_GLSL_COMPILER}")

function(mv_compile_shaders TARGET)
    set(_spv_files "")
    set(_out_dir "${CMAKE_BINARY_DIR}/shaders")
    file(MAKE_DIRECTORY "${_out_dir}")

    foreach(_src IN LISTS ARGN)
        get_filename_component(_abs "${_src}" ABSOLUTE)
        get_filename_component(_name "${_src}" NAME)
        set(_spv "${_out_dir}/${_name}.spv")

        get_filename_component(_dir "${_abs}" DIRECTORY)

        if(_mv_glsl_name STREQUAL "glslc")
            set(_cmd "${MV_GLSL_COMPILER}" --target-env=vulkan1.3 -O -g
                     "-I${_dir}" "${_abs}" -o "${_spv}")
        else()
            set(_cmd "${MV_GLSL_COMPILER}" --target-env vulkan1.3
                     "-I${_dir}" -o "${_spv}" "${_abs}")
        endif()

        file(GLOB _mv_shader_headers "${_dir}/*.glsl")

        add_custom_command(
            OUTPUT  "${_spv}"
            COMMAND ${_cmd}
            DEPENDS "${_abs}" ${_mv_shader_headers}
            COMMENT "GLSL -> SPIR-V: ${_name}"
            VERBATIM)

        list(APPEND _spv_files "${_spv}")
    endforeach()

    add_custom_target(${TARGET}_shaders DEPENDS ${_spv_files})
    add_dependencies(${TARGET} ${TARGET}_shaders)

    # Multi-config generators put the binary in a per-config subdirectory,
    # so copy after the build rather than compiling straight into place.
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:${TARGET}>/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_spv_files}
                "$<TARGET_FILE_DIR:${TARGET}>/shaders"
        COMMENT "Staging SPIR-V next to ${TARGET}"
        VERBATIM)
endfunction()
