# Cross-compile to Windows from Linux with MinGW-w64.
#
#   cmake -S . -B build/win-cross -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake \
#         -DCMAKE_BUILD_TYPE=Release \
#         -DMV_VULKAN_SDK_WIN=/path/to/windows/VulkanSDK/1.3.x.y
#   cmake --build build/win-cross
#
# Read this before relying on it
# ------------------------------
# This works, but it is the harder of the two routes. Everything the project
# depends on is built from source, so there is no MSVC/MinGW ABI clash -- the
# friction is entirely Vulkan:
#
#   * There is no Linux package that provides the *Windows* Vulkan headers or
#     the vulkan-1 import library. You have to point this file at an extracted
#     Windows SDK, or let it fetch Vulkan-Headers and generate an import
#     library from a .def file (done below).
#   * MinGW builds of Draco and Assimp see far less testing than MSVC ones.
#   * You cannot run or debug the result without Wine.
#
# For releases, building on a real Windows runner (see
# .github/workflows/build.yml) is more reliable and needs no special casing.
# This file exists for quick local checks that Windows compilation has not
# regressed.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MV_MINGW_PREFIX x86_64-w64-mingw32 CACHE STRING "MinGW-w64 target triple")

set(CMAKE_C_COMPILER   ${MV_MINGW_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${MV_MINGW_PREFIX}-g++)
set(CMAKE_RC_COMPILER  ${MV_MINGW_PREFIX}-windres)
set(CMAKE_AR           ${MV_MINGW_PREFIX}-ar)
set(CMAKE_RANLIB       ${MV_MINGW_PREFIX}-ranlib)

set(CMAKE_FIND_ROOT_PATH /usr/${MV_MINGW_PREFIX})

# Look for headers and libraries only in the target sysroot, but run host
# programs (glslangValidator, git) from the host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Static libgcc/libstdc++ so the result does not need MinGW runtime DLLs
# alongside it.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++ -static")

# --- Vulkan ----------------------------------------------------------------
# If an extracted Windows SDK is supplied, use it directly.
if(DEFINED MV_VULKAN_SDK_WIN)
    set(Vulkan_INCLUDE_DIR "${MV_VULKAN_SDK_WIN}/Include" CACHE PATH "")
    set(Vulkan_LIBRARY     "${MV_VULKAN_SDK_WIN}/Lib/vulkan-1.lib" CACHE FILEPATH "")
    set(Vulkan_GLSLC_EXECUTABLE "${MV_VULKAN_SDK_WIN}/Bin/glslc.exe" CACHE FILEPATH "")
endif()

# The shader compiler runs on the host, so prefer a native one if present.
find_program(MV_HOST_GLSLANG glslangValidator)
if(MV_HOST_GLSLANG)
    set(MV_GLSL_COMPILER "${MV_HOST_GLSLANG}" CACHE FILEPATH "" FORCE)
endif()
