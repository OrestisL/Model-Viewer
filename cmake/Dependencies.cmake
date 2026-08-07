# ---------------------------------------------------------------------------
# Third-party dependencies.
#
# Everything except the Vulkan SDK is pulled in with FetchContent, so a fresh
# clone only needs: a compiler, CMake, git and the LunarG Vulkan SDK.
#
# Set FETCHCONTENT_SOURCE_DIR_<NAME> to point at a local checkout, or
# -DFETCHCONTENT_FULLY_DISCONNECTED=ON for offline builds.
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- Vulkan SDK ------------------------------------------------------------
# Vulkan::Vulkan and (when the SDK is installed) Vulkan::glslc / Vulkan::glslangValidator
find_package(Vulkan REQUIRED)

# --- GLFW ------------------------------------------------------------------
set(GLFW_BUILD_DOCS      OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL         OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE)

# --- GLM -------------------------------------------------------------------
set(GLM_ENABLE_CXX_20 ON CACHE BOOL "" FORCE)
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    TRUE)

# --- Assimp ----------------------------------------------------------------
# Import *and* export are enabled; the format lists below trim build time while
# keeping every format the viewer advertises.
set(ASSIMP_BUILD_TESTS            OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS     OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES          OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL                OFF CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS     OFF CACHE BOOL "" FORCE)
set(ASSIMP_INJECT_DEBUG_POSTFIX   OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ZLIB             ON  CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT              OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS             OFF CACHE BOOL "" FORCE)
FetchContent_Declare(assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG        v5.4.3
    GIT_SHALLOW    TRUE)

# --- vk-bootstrap (instance/device/swapchain boilerplate) -------------------
FetchContent_Declare(vk_bootstrap
    GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
    GIT_TAG        v1.3.290
    GIT_SHALLOW    TRUE)

# --- VulkanMemoryAllocator -------------------------------------------------
FetchContent_Declare(vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.1.0
    GIT_SHALLOW    TRUE)

# --- Dear ImGui (docking branch: dockspace + viewports) --------------------
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.8-docking
    GIT_SHALLOW    TRUE)

# --- stb (image decoding for embedded/external textures) -------------------
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        f0569113c93ad095470c54bf34a17b36646bbbb5)

# --- ImGuizmo (translate/rotate/scale manipulator) -------------------------
# SOURCE_SUBDIR points at a directory that does not exist so MakeAvailable
# populates the sources without running ImGuizmo's own CMakeLists.
# Pinned to a commit rather than a branch: ImGuizmo has no recent release tag,
# and master has moved its sources between directories before now.
FetchContent_Declare(imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG        5ab7676402ace03cdf930b2d972f59c7d03c6fa8
    SOURCE_SUBDIR  no-cmake-here)

# Draco-compressed glTF.
#
# KHR_draco_mesh_compression is common in the wild -- Sketchfab and most web
# pipelines emit it -- and without the decoder those files load with no
# geometry at all rather than failing loudly. Draco ships vendored inside
# Assimp, so this is only an option flip, not another dependency to fetch.
if(MV_ENABLE_DRACO)
    set(ASSIMP_BUILD_DRACO        ON CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_DRACO_STATIC ON CACHE BOOL "" FORCE)
    message(STATUS "Draco glTF decompression enabled")
endif()

# USD goes through Assimp's own importer rather than a separate backend.
#
# Assimp 5.4 ships an (experimental) USD reader that clones a pinned tinyusdz
# revision into its contrib tree at configure time. Using it means USD files
# travel the same path as every other format -- same material handling, same
# scene graph, same code -- instead of needing a parallel implementation that
# would have to be kept in step.
#
# It must be set before assimp is made available, which is why this sits here.
if(MV_ENABLE_USD)
    set(ASSIMP_BUILD_USD_IMPORTER ON  CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_USD_VERBOSE_LOGS OFF CACHE BOOL "" FORCE)
    message(STATUS "USD import enabled (Assimp + tinyusdz -- adds several minutes to the build)")
endif()

# tinyusdz relies on <cstdint> arriving transitively.
#
# 63 of its files use uint32_t / uint16_t / uint8_t without including
# <cstdint>. That worked for years because libstdc++ pulled the header in
# through <vector>, <memory> and friends. GCC 13 and 14 progressively stopped
# doing that, so on a current toolchain every declaration in tinyusdz::value
# fails -- and because those types underpin the whole library, one missing
# include cascades into roughly a thousand template errors elsewhere that look
# nothing like the cause.
#
# Force-including the header fixes all 63 files at once without touching
# third-party sources. CMAKE_CXX_FLAGS applies to C++ only, which matters:
# tinyusdz compiles lz4.c into the same target and <cstdint> is not valid C.
# The flag is restored afterwards, so it applies to the dependencies rather
# than to our own code.
# Applied to every dependency, not just tinyusdz: Draco 1.4 predates the same
# libstdc++ change and relies on the same transitive <cstdint>. Force-including
# it is harmless where it is already present and pre-empts an error whose
# symptom (a thousand template failures) looks nothing like its cause.
set(MV_SAVED_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
if(MSVC)
    string(APPEND CMAKE_CXX_FLAGS " /FIcstdint")
else()
    string(APPEND CMAKE_CXX_FLAGS " -include cstdint")
endif()

FetchContent_MakeAvailable(glfw glm assimp vk_bootstrap vma imgui stb imguizmo)

set(CMAKE_CXX_FLAGS "${MV_SAVED_CXX_FLAGS}")

# --- Header-only targets ---------------------------------------------------
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${stb_SOURCE_DIR})

# --- Dear ImGui has no CMake of its own ------------------------------------
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)

target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)

target_link_libraries(imgui PUBLIC glfw Vulkan::Vulkan)

# Let the backend pull function pointers through the loader we already have.
# Note: IMGUI_IMPL_VULKAN_NO_PROTOTYPES must NOT be defined here, not even to
# 0 -- imgui_impl_vulkan.h tests it with #if defined(), so any definition puts
# the backend into custom-loader mode and trips its g_FunctionsLoaded assert.
target_compile_definitions(imgui PUBLIC
    IMGUI_DEFINE_MATH_OPERATORS)

# glfw3.h includes <GL/gl.h> unless this is set; GLFW_INCLUDE_VULKAN does not
# suppress it. A Vulkan-only machine has no reason to have the GL headers.
target_compile_definitions(imgui PRIVATE GLFW_INCLUDE_NONE)

# ImGuizmo compiles into the imgui target: it needs imgui_internal.h and has
# no other dependencies.
# Newer ImGuizmo keeps its sources under src/; older layouts have them at the
# repository root. Support both so a re-pin does not break the build.
if(EXISTS "${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp")
    set(MV_IMGUIZMO_DIR "${imguizmo_SOURCE_DIR}/src")
elseif(EXISTS "${imguizmo_SOURCE_DIR}/ImGuizmo.cpp")
    set(MV_IMGUIZMO_DIR "${imguizmo_SOURCE_DIR}")
else()
    message(FATAL_ERROR "Could not locate ImGuizmo.cpp under ${imguizmo_SOURCE_DIR}")
endif()

target_sources(imgui PRIVATE ${MV_IMGUIZMO_DIR}/ImGuizmo.cpp)
target_include_directories(imgui PUBLIC ${MV_IMGUIZMO_DIR})

# --- Optional: USD via tinyusdz -------------------------------------------
