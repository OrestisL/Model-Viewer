# ModelViewer

A cross-platform 3D model viewer built on **Vulkan 1.3**, **Dear ImGui** and **Assimp**.
Builds with CMake on Linux and Windows.

---

## Features

| Requirement | Status |
|---|---|
| Read FBX, OBJ, STL, glTF, GLB (+ DAE, PLY, 3DS, BLEND, …) | Assimp importer |
| Read USD / USDA / USDC / USDZ | Assimp + tinyusdz, `-DMV_ENABLE_USD=ON` |
| Draco-compressed glTF/GLB | Yes, on by default (`MV_ENABLE_DRACO`) |
| meshopt-compressed glTF/GLB | **Not supported** — see Known gaps |
| Skeletal + node animation playback | Timeline, scrub, loop, speed, clip selection |
| Texture display | Base colour, metal-rough, normal, emissive, occlusion (embedded or external) |
| Fallback colour with a colour wheel | Rendering panel → hue-wheel picker |
| Import lights | Directional / point / spot, with a default 3-point rig when the file has none |
| Import cameras | Listed in the UI, click to adopt the camera's transform and FOV |
| Export to other formats | Every Assimp export target: fbx, obj, stl, ply, gltf2, glb2, collada, x3d, 3ds, … |

Plus: PBR metallic-roughness shading, infinite grid, orbit/pan/zoom camera, drag-and-drop
loading, scene-graph inspector, material inspector, wireframe and normal-debug views,
and an in-app log panel.

---

## Prerequisites

- **CMake ≥ 3.24**
- **A C++20 compiler** — GCC 11+, Clang 14+, or MSVC 2022
- **Git** (dependencies are fetched at configure time)
- **[LunarG Vulkan SDK](https://vulkan.lunarg.com/)** — this is the one dependency that
  is *not* fetched automatically. It supplies the Vulkan headers, the validation layers
  and `glslc`.
- A GPU and driver supporting **Vulkan 1.3** (dynamic rendering, synchronization2,
  descriptor indexing).

On Debian/Ubuntu you will also want the usual X11/Wayland development headers that
GLFW needs:

```bash
sudo apt install build-essential cmake git \
     libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
     libwayland-dev libxkbcommon-dev wayland-protocols
```

Everything else — GLFW, GLM, Assimp, Dear ImGui, VMA, vk-bootstrap, stb — is pulled in
by `FetchContent`. The first configure takes a while (Assimp is the bulk of it);
subsequent ones are cached.

---

## Building

### Linux / macOS

```bash
git clone <this-repo> ModelViewer && cd ModelViewer
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
./build/bin/ModelViewer path/to/model.glb
```

### Windows (Visual Studio 2022)

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo
.\build\bin\RelWithDebInfo\ModelViewer.exe
```

### Windows (Ninja / clang-cl / MSVC command line)

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

### CMake options

| Option | Default | Meaning |
|---|---|---|
| `MV_ENABLE_VALIDATION` | `ON` | Vulkan validation layers in Debug/RelWithDebInfo |
| `MV_ENABLE_DRACO` | `ON` | Decode `KHR_draco_mesh_compression` (adds ~2 min to the build) |
| `MV_ENABLE_USD` | `OFF` | Enable USD import via Assimp's tinyusdz reader (adds ~10 min to the build) |
| `MV_WARNINGS_AS_ERRORS` | `OFF` | `-Werror` / `/WX` |

If CMake cannot find the shader compiler, point it at one explicitly:

```bash
cmake -S . -B build -DMV_GLSL_COMPILER=/path/to/glslc
```

---

## Usage

```
ModelViewer [options] [model-file]

  -h, --help      show usage
  -v, --version   show version
```

Or just drag a file onto the window.

### Controls

| Input | Action |
|---|---|
| Left drag | Orbit |
| Middle drag / Shift + left drag | Pan |
| Right drag / scroll | Dolly |
| `T` | Show / hide the side panel |
| `F` | Frame the model |
| `Space` | Play / pause animation |
| `W` | Toggle wireframe |
| `G` | Toggle grid |
| `Esc` | Quit |

### Exporting

`File → Export As…` lists every format the linked Assimp build can write. The exporter
writes the **originally imported scene graph**, not a re-derived one, so round-trips
(GLB → FBX, FBX → glTF) keep hierarchy, skins and animation data intact.

Two caveats worth knowing:

- Assimp's FBX writer emits FBX 7.4 binary/ASCII. It is solid for meshes, materials and
  hierarchy, but its animation and skin support is weaker than its reader's. Check the
  result before relying on it in a pipeline.
- USD is **import-only** — tinyusdz has no writer wired up. Since USD now goes through
  Assimp, a loaded USD scene *can* be exported to any other format Assimp writes
  (glTF, FBX, OBJ, ...), which was not true of the old standalone backend.

---

## File associations (Linux)

Most 3D formats have no registered MIME type, so a stock system does not know
what a `.fbx` or `.usd` is and there is nothing for a desktop entry to attach
to. `resources/modelviewer-mime.xml` declares the missing types (with
content-sniffing rules where a format has a usable signature, such as FBX's
`Kaydara FBX Binary` header and USD crate's `PXR-USDC`).

Note that declaring the app *can* open a type is separate from making it the
*default* for that type. The script does both:

```bash
bash scripts/register-file-associations.sh                # per-user, into ~/.local
sudo bash scripts/register-file-associations.sh /usr/local
```

Invoked through `bash` because archives do not reliably preserve the execute
bit; `chmod +x scripts/register-file-associations.sh` works equally well.

Prefer the per-user form. `xdg-mime default` writes to the *calling* user's
`~/.config/mimeapps.list`, so running the whole script under `sudo` sets
root's defaults and leaves yours untouched.

Verify:

```bash
xdg-mime query filetype model.fbx     # -> model/x.fbx
xdg-mime query default  model/x.fbx   # -> modelviewer.desktop
```

`cmake --install` also installs the MIME package and refreshes both caches, but
does not change your defaults -- an install should not silently take over file
types.

## Layout

```
CMakeLists.txt            top-level build
cmake/
  Dependencies.cmake      all FetchContent declarations
  CompileShaders.cmake    GLSL -> SPIR-V, staged next to the executable
shaders/
  common.glsl             shared UBO/SSBO layouts + PBR helpers
  mesh.vert/.frag         skinned PBR forward pass
  grid.vert/.frag         screen-space infinite grid
src/
  main.cpp                entry point
  App.{hpp,cpp}           frame loop, wiring, load/export actions
  third_party_impl.cpp    STB_IMAGE_IMPLEMENTATION + VMA_IMPLEMENTATION
  core/                   Window (GLFW), Camera (orbit), Log
  vk/
    Context.{hpp,cpp}     instance/device/swapchain via vk-bootstrap, VMA allocator
    Resources.{hpp,cpp}   buffer/image helpers, staging uploads, mip generation
    Renderer.{hpp,cpp}    pipelines, descriptors, per-frame data, draw submission
  scene/
    Scene.hpp             engine-agnostic scene representation
    ModelLoader.{hpp,cpp} Assimp import + export
    UsdBackend.{hpp,cpp}  USD extension list + no-USD-support message
    Animator.{hpp,cpp}    clip sampling, node TRS, joint palettes
  ui/
    UiLayer.{hpp,cpp}     all ImGui panels
    FileBrowser.{hpp,cpp} portable open/save dialog
```

### Design notes

**Why Vulkan 1.3 specifically.** The renderer uses dynamic rendering and
synchronization2, which removes render-pass and framebuffer objects entirely, and
descriptor indexing for a bindless texture array. That last one is what lets the whole
scene draw from a single descriptor set with the material index in a push constant,
instead of rebinding per draw call.

**Why ImGui rather than Qt.** Immediate-mode fits a viewer's "the UI is a view onto
mutable render state" shape, it has a first-party Vulkan backend that renders into the
same command buffer, and it adds no deployment or licensing burden. The trade-off is
that it is not a native-looking application; if you need platform-native chrome, Qt is
the better choice and the `ui/` layer is the only part you would rewrite.

**Scene representation.** `Scene` is deliberately independent of both Assimp and Vulkan
— flat arrays of nodes, meshes, materials, skins, lights, cameras and animation clips.
`ModelLoader` and `UsdBackend` both produce one; `Renderer` consumes one. Adding a third
importer means adding one file.

**Skinning** runs in the vertex shader against a joint-matrix SSBO that the animator
refills each frame. Meshes without skins take the same path with an identity palette,
which keeps the pipeline count down.

---

## Shipping it

### Prebuilt binaries

`.github/workflows/build.yml` builds Release binaries for Linux and Windows on
every push, and attaches packaged archives to a GitHub Release when a `v*` tag
is pushed:

```bash
git tag v0.1.0 && git push origin v0.1.0
```

Windows is built on a real Windows runner with MSVC rather than cross-compiled,
which avoids a class of problems that is not worth debugging for a release
artefact. Dependencies are cached against the hash of `cmake/Dependencies.cmake`,
so a run only pays the full ~20 minute dependency build when a pin changes.

### What a user needs

Only a GPU and driver supporting **Vulkan 1.3**. Everything else — Assimp,
GLFW, ImGui, Draco — is compiled in, and `MV_STATIC_RUNTIME` (on in the `-dist`
presets) links the C++ runtime statically, so there is no VC++ redistributable
or `libstdc++` version to match.

The Vulkan loader is deliberately **not** bundled: it belongs to the user's
graphics driver, and shipping your own is a reliable way to break machines that
differ from your own.

### Cross-compiling to Windows from Linux

Possible, but the harder route:

```bash
sudo apt install mingw-w64
cmake -S . -B build/win-cross -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DMV_VULKAN_SDK_WIN=/path/to/extracted/windows/VulkanSDK
cmake --build build/win-cross
```

The obstacle is Vulkan, not the compiler: no Linux package ships the *Windows*
Vulkan headers or the `vulkan-1` import library, so the toolchain file has to be
pointed at an extracted Windows SDK. MinGW builds of Assimp and Draco also see
far less testing than MSVC ones, and you cannot run the result without Wine.
Use it for a quick check that Windows compilation still works; use CI for
anything you intend to hand to someone.

## Known gaps

This is a solid foundation rather than a finished product. The honest list:

- No shadow mapping. Lights affect shading but cast nothing.
- No IBL/environment map — ambient is a flat term. Adding a prefiltered cubemap is the
  single biggest visual improvement available.
- `EXT_meshopt_compression` is not decoded. Assimp has no support for it, so such
  files load with missing or empty geometry. `gltfpack -i in.glb -o out.glb -noq`
  round-trips them to an uncompressed file that loads fine.
- Morph targets are parsed but not evaluated.
- No MSAA; the swapchain is the render target directly.
- Animation blending between clips is not implemented (one active clip at a time).
- Assimp's USD reader needs a `<cstdint>` force-include on GCC 13+ (handled in
  `cmake/Dependencies.cmake`); tinyusdz relies on that header arriving transitively,
  which modern libstdc++ no longer guarantees.
- Assimp's USD reader is marked experimental upstream. Meshes, transforms and basic
  UsdPreviewSurface materials come through; skinning, USD animation and instancing
  are unreliable or unsupported.

I have not been able to compile this in the environment I wrote it in — there is no
Vulkan SDK or GPU here. The structure, CMake wiring and shader/host interface layouts
are consistent, but budget an hour for first-build fixes, and build with validation
layers on the first time you run it.
