cmake_minimum_required(VERSION 3.24)

project(ModelViewer
    VERSION 1.0.0
    DESCRIPTION "Cross-platform Vulkan 3D model viewer"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(MV_ENABLE_VALIDATION "Enable Vulkan validation layers in debug builds" ON)
option(MV_STATIC_RUNTIME "Link the C/C++ runtime statically for redistribution" OFF)
option(MV_ENABLE_BLEND      "Build Assimp's .blend importer (unmaintained, crashes on modern files)" OFF)
option(MV_ENABLE_DRACO      "Decode Draco-compressed glTF/GLB (adds ~2 min to the build)" ON)
option(MV_ENABLE_USD        "Enable USD/USDA/USDC/USDZ import (Assimp + tinyusdz)" OFF)
option(MV_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# The MSVC runtime library must be identical across every target linked into
# the same binary. Setting it on our executable alone leaves GLFW, Assimp and
# ImGui on the default dynamic CRT, and the link fails with LNK2038. It has to
# be set here, before the dependencies are configured, so they inherit it.
if(MV_STATIC_RUNTIME AND MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
include(Dependencies)
include(CompileShaders)

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
set(MV_SOURCES
    src/main.cpp
    src/App.cpp
    src/third_party_impl.cpp

    src/core/Window.cpp
    src/core/Camera.cpp
    src/core/Crash.cpp
    src/core/Utf8.cpp

    src/vk/Context.cpp
    src/vk/Resources.cpp
    src/vk/Renderer.cpp
    src/vk/SplatRenderer.cpp
    src/vk/SplatSorter.cpp

    src/scene/Scene.cpp
    src/scene/History.cpp
    src/scene/ModelLoader.cpp
    src/scene/SplatLoader.cpp
    src/scene/Animator.cpp
    src/scene/UsdBackend.cpp

    src/ui/Theme.cpp
    src/ui/UiLayer.cpp
    src/ui/FileBrowser.cpp
)

add_executable(ModelViewer ${MV_SOURCES})

target_include_directories(ModelViewer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(ModelViewer PRIVATE
    Vulkan::Vulkan
    vk-bootstrap::vk-bootstrap
    GPUOpen::VulkanMemoryAllocator
    glfw
    glm::glm
    assimp::assimp
    imgui
    stb)

# -- zlib, for SPZ (Gaussian-splat) gzip decompression -----------------------
# SPZ files are gzip streams, so SplatLoader.cpp needs an inflate routine.
# Assimp is fetched with ASSIMP_BUILD_ZLIB ON, which builds a `zlibstatic`
# target in this same build tree -- reuse it so no new external dependency is
# introduced. Fall back to a system zlib if that target isn't present (e.g. if
# Assimp is ever swapped for a system package), and error clearly otherwise.
if(TARGET zlibstatic)
    target_link_libraries(ModelViewer PRIVATE zlibstatic)
    set(MV_ZLIB_TARGET zlibstatic)
elseif(TARGET ZLIB::ZLIB)
    target_link_libraries(ModelViewer PRIVATE ZLIB::ZLIB)
    set(MV_ZLIB_TARGET ZLIB::ZLIB)
else()
    find_package(ZLIB REQUIRED)
    target_link_libraries(ModelViewer PRIVATE ZLIB::ZLIB)
    set(MV_ZLIB_TARGET ZLIB::ZLIB)
endif()

target_compile_definitions(ModelViewer PRIVATE
    GLFW_INCLUDE_VULKAN
    GLFW_INCLUDE_NONE
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
    MV_SHADER_DIR="shaders"
    $<$<BOOL:${MV_ENABLE_USD}>:MV_USD_VIA_ASSIMP=1>
    $<$<BOOL:${MV_ENABLE_DRACO}>:MV_WITH_DRACO=1>
    $<$<AND:$<BOOL:${MV_ENABLE_VALIDATION}>,$<CONFIG:Debug,RelWithDebInfo>>:MV_VALIDATION=1>)

# -- optional standalone splat loader validation tool ------------------------
# Builds without Vulkan: compiles SplatLoader against the real headers and
# prints a summary of any .ply/.spz splat file. Handy for validating the loader
# on real data. Enable with -DMV_BUILD_SPLAT_TOOLS=ON.
option(MV_BUILD_SPLAT_TOOLS "Build the standalone splat_inspect tool" OFF)
if(MV_BUILD_SPLAT_TOOLS)
    add_executable(splat_inspect
        tools/splat_inspect.cpp
        src/scene/SplatLoader.cpp
        src/core/Utf8.cpp)
    target_include_directories(splat_inspect PRIVATE src)
    target_link_libraries(splat_inspect PRIVATE glm::glm ${MV_ZLIB_TARGET})
    target_compile_features(splat_inspect PRIVATE cxx_std_20)

    # Validates the GPU radix sort's algorithm on the CPU against real clouds.
    add_executable(radix_ref
        tools/radix_ref.cpp
        src/scene/SplatLoader.cpp
        src/core/Utf8.cpp)
    target_include_directories(radix_ref PRIVATE src)
    target_link_libraries(radix_ref PRIVATE glm::glm ${MV_ZLIB_TARGET})
    target_compile_features(radix_ref PRIVATE cxx_std_20)

    # Validates the spherical-harmonics colour evaluation on real clouds.
    add_executable(sh_ref
        tools/sh_ref.cpp
        src/scene/SplatLoader.cpp
        src/core/Utf8.cpp)
    target_include_directories(sh_ref PRIVATE src)
    target_link_libraries(sh_ref PRIVATE glm::glm ${MV_ZLIB_TARGET})
    target_compile_features(sh_ref PRIVATE cxx_std_20)
endif()

if(MSVC)
    target_compile_options(ModelViewer PRIVATE /W4 /permissive- /utf-8 /MP
        $<$<BOOL:${MV_WARNINGS_AS_ERRORS}>:/WX>)
    target_compile_definitions(ModelViewer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        PROPERTY VS_STARTUP_PROJECT ModelViewer)
    # Console window is handy for logs; switch to WIN32 for a windowed-only build.
else()
    target_compile_options(ModelViewer PRIVATE -Wall -Wextra -Wpedantic
        -Wno-unused-parameter -Wno-missing-field-initializers
        $<$<BOOL:${MV_WARNINGS_AS_ERRORS}>:-Werror>)
endif()

# tinyusdz is compiled into the assimp target when ASSIMP_BUILD_USD_IMPORTER
# is on, so there is nothing extra to link here.

# ---------------------------------------------------------------------------
# Shaders -> SPIR-V, emitted next to the executable
# ---------------------------------------------------------------------------
mv_compile_shaders(ModelViewer
    shaders/mesh.vert
    shaders/mesh.frag
    shaders/grid.vert
    shaders/grid.frag
    shaders/shadow.vert
    shaders/sky.vert
    shaders/sky.frag
    shaders/axes.vert
    shaders/axes.frag
    shaders/splat.vert
    shaders/splat.frag
    shaders/splat_sort_key.comp
    shaders/splat_sort_histogram.comp
    shaders/splat_sort_scan.comp
    shaders/splat_sort_scatter.comp)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Resources (icon, desktop entry)
# ---------------------------------------------------------------------------
target_compile_definitions(ModelViewer PRIVATE
    MV_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources")

# Stage resources beside the executable so a packaged build is self-contained.
add_custom_command(TARGET ModelViewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ModelViewer>/resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.png"
            "$<TARGET_FILE_DIR:ModelViewer>/resources/"
    COMMENT "Staging resources"
    # VERBATIM makes CMake escape the arguments for the shell it will actually
    # use. Without it the paths are pasted in raw, and any that contain shell
    # metacharacters -- a bracket, a space, an ampersand -- produce a syntax
    # error at build time rather than anything that names the cause.
    VERBATIM)

if(WIN32)
    # Compiled into the .exe, so Explorer and the taskbar show it before the
    # program runs. Separate from the runtime GLFW window icon.
    target_sources(ModelViewer PRIVATE resources/ModelViewer.rc)

    # A GUI subsystem binary has no console window. Debug builds keep the
    # console so log output is visible.
    set_target_properties(ModelViewer PROPERTIES
        WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>)

    # /SUBSYSTEM:WINDOWS makes the linker look for WinMain, but this program
    # has an ordinary main(). Pointing it at the console CRT entry point keeps
    # the standard signature while still producing a windowed binary. MinGW
    # needs none of this: it supplies its own WinMain that forwards to main().
    if(MSVC)
        target_link_options(ModelViewer PRIVATE
            $<$<NOT:$<CONFIG:Debug>>:/ENTRY:mainCRTStartup>)
    endif()
endif()

if(NOT MSVC)
    # Export dynamic symbols so the crash handler's backtrace can name
    # functions instead of printing raw addresses.
    target_link_options(ModelViewer PRIVATE -rdynamic)
endif()

if(MV_STATIC_RUNTIME AND NOT MSVC)
    target_link_options(ModelViewer PRIVATE -static-libstdc++ -static-libgcc)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS ModelViewer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/shaders/"
        DESTINATION "${CMAKE_INSTALL_BINDIR}/shaders")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.png"
        DESTINATION "${CMAKE_INSTALL_BINDIR}/resources")

if(WIN32)
    install(FILES $<TARGET_RUNTIME_DLLS:ModelViewer>
            DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
    # Freedesktop integration: the compositor matches StartupWMClass against
    # the window's app id to find this icon.
    # The desktop entry needs an absolute Exec path, so it is generated.
    set(MV_EXEC_PATH "${CMAKE_INSTALL_FULL_BINDIR}/ModelViewer")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/modelviewer.desktop.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/modelviewer.desktop" @ONLY)

    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/modelviewer.desktop"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.png"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/256x256/apps"
            RENAME modelviewer.png)

    # MIME type definitions. Without these the desktop entry references types
    # the system has never heard of, and nothing associates.
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/modelviewer-mime.xml"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/mime/packages")

    # Rebuild the caches on install, unless packaging into a staging dir.
    # Rebuild the MIME and desktop caches after install. A bracket argument is
    # used so nothing needs escaping and every ${} resolves at install time,
    # where CMAKE_INSTALL_PREFIX is what the user actually installed to.
    # Skipped under DESTDIR, where the tree is being staged for a package and
    # the caches belong to the target system, not this one.
    install(CODE [[
        if(NOT DEFINED ENV{DESTDIR})
            find_program(MV_UPDATE_MIME update-mime-database)
            if(MV_UPDATE_MIME)
                execute_process(
                    COMMAND "${MV_UPDATE_MIME}" "${CMAKE_INSTALL_PREFIX}/share/mime"
                    ERROR_QUIET)
                message(STATUS "Updated MIME database")
            endif()

            find_program(MV_UPDATE_DESKTOP update-desktop-database)
            if(MV_UPDATE_DESKTOP)
                execute_process(
                    COMMAND "${MV_UPDATE_DESKTOP}" "${CMAKE_INSTALL_PREFIX}/share/applications"
                    ERROR_QUIET)
                message(STATUS "Updated desktop database")
            endif()
        endif()
    ]])
endif()

# ---------------------------------------------------------------------------
# Packaging: cmake --build . --target package
# ---------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME              "ModelViewer")
set(CPACK_PACKAGE_VENDOR            "ModelViewer")
# CI passes the git tag so the archive name matches the release. Without it
# the file keeps whatever version is hardcoded above, which is how a v1.0.0
# release ends up full of ModelViewer-0.1.0-* files.
set(MV_VERSION "${PROJECT_VERSION}" CACHE STRING "Version stamped into packages")
set(CPACK_PACKAGE_VERSION           "${MV_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Vulkan 3D model viewer and converter")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "ModelViewer")
# CMAKE_SYSTEM_PROCESSOR reports x86_64 on Linux and AMD64 on Windows for the
# same hardware, so it is normalised rather than passed through -- the same
# machine should not produce two differently-named architectures.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(MV_ARCH "x86_64")
else()
    set(MV_ARCH "x86")
endif()

if(WIN32)
    set(MV_PLATFORM "windows")
elseif(APPLE)
    set(MV_PLATFORM "macos")
else()
    set(MV_PLATFORM "linux")
endif()

set(CPACK_PACKAGE_FILE_NAME "ModelViewer-${MV_VERSION}-${MV_PLATFORM}-${MV_ARCH}")
set(CPACK_VERBATIM_VARIABLES ON)
set(CPACK_STRIP_FILES ON)

if(WIN32)
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)
