cmake_minimum_required(VERSION 3.24)

project(ModelViewer
    VERSION 0.1.0
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

    src/vk/Context.cpp
    src/vk/Resources.cpp
    src/vk/Renderer.cpp

    src/scene/Scene.cpp
    src/scene/History.cpp
    src/scene/ModelLoader.cpp
    src/scene/Animator.cpp
    src/scene/UsdBackend.cpp

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
    shaders/axes.frag)

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
    COMMENT "Staging resources")

if(WIN32)
    # Compiled into the .exe, so Explorer and the taskbar show it before the
    # program runs. Separate from the runtime GLFW window icon.
    target_sources(ModelViewer PRIVATE resources/ModelViewer.rc)

    # A GUI subsystem binary has no console window. Debug builds keep the
    # console so log output is visible.
    set_target_properties(ModelViewer PROPERTIES
        WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>)
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
set(CPACK_PACKAGE_VERSION           "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Vulkan 3D model viewer and converter")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "ModelViewer")
set(CPACK_PACKAGE_FILE_NAME
    "ModelViewer-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_VERBATIM_VARIABLES ON)
set(CPACK_STRIP_FILES ON)

if(WIN32)
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)
