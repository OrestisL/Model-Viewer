# ModelViewer

A cross-platform 3D model viewer built on **Vulkan 1.3**, **Dear ImGui** and **Assimp**.
Builds with CMake on Linux and Windows.

---

## Features

| Requirement | Status |
|---|---|
| Read FBX, OBJ, STL, glTF, GLB (+ DAE, PLY, 3DS, X, LWO, OFF, …) | Assimp importer |
| Read USD / USDA / USDC / USDZ | Assimp + tinyusdz, `compile with -DMV_ENABLE_USD=ON` |
| Draco-compressed glTF/GLB | Yes, on by default (`MV_ENABLE_DRACO`) |
| Configurable sky | Gradient sky with presets, optional sun disc |
| Ambient light | Colour and strength configurable, or derived from the sky per-normal |
| Unlit mode | Bypasses all lighting for models with baked-in lighting |
| Per-node visibility | Hide meshes with `H`, restore all with `Shift`+`H`, or toggle in the tree |
| Isolate selection | `I` shows only the selection and its children; leaving restores what was showing before |
| Undo / redo | `Ctrl`+`Z` / `Ctrl`+`Shift`+`Z` or `Crtl` + `Y` for transforms and visibility changes |
| Shadows | PCF-filtered shadow map, cast by the brightest light in the scene |
| meshopt-compressed glTF/GLB | **Not supported** — run `gltfpack -noq` to decompress first |
| Skeletal + node animation playback | Timeline, scrub, loop, speed, clip selection |
| Texture display | Base colour, metal-rough, normal, emissive, occlusion (embedded or external) |
| Fallback colour with a colour wheel | Rendering panel → hue-wheel picker |
| Import lights | Directional / point / spot, with a default 3-point rig when the file has none |
| Import cameras | Listed in the UI, click to adopt the camera's transform and FOV |
| Export to other formats | Every Assimp export target: fbx, obj, stl, ply, gltf2, glb2, collada, x3d, 3ds, … |

Plus: PBR metallic-roughness shading, 4x MSAA, RGB origin axes, 
a transform gumball with undo/redo, click-to-select picking, orbit/pan/zoom camera,
drag-and-drop loading, scene-graph and material inspectors, wireframe and normal-debug
views, and an in-app log panel.

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

## Downloads

Prebuilt binaries for Linux and Windows are attached to each
[release](../../releases). Extract and run — no installer, no runtime to
install. The only requirement is a GPU and driver supporting **Vulkan 1.3**.


## Building

### Linux / macOS

```bash
git clone <this-repo> ModelViewer && cd ModelViewer
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
./build/bin/ModelViewer path/to/model.glb
```

### Windows (Visual Studio)

The generator is deliberately not pinned: CMake picks whichever Visual Studio
is installed, and VS 2019 and later already default to the host architecture.
Naming a version only guarantees the build breaks when the toolchain moves on.

```powershell
cmake -S . -B build
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
| `MV_ENABLE_USD` | `OFF` | Enable USD import via Assimp's tinyusdz reader |
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
| `H` | Hide the selected mesh |
| `Shift` + `H` | Show all meshes |
| `I` | Isolate the selection / leave isolate |
| `F` | Frame the model |
| `Space` | Play / pause animation |
| `W` | Toggle wireframe |
| `G` | Toggle grid |
| `Esc` | Quit |

### Exporting

`File → Export As…` lists every format the linked Assimp build can write. The exporter
writes the **originally imported scene graph**, not a re-derived one, so round-trips
(GLB → FBX, FBX → glTF) keep hierarchy, skins and animation data intact.

Notes:

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

**Scene representation.** `Scene` is deliberately independent of both Assimp and Vulkan
— flat arrays of nodes, meshes, materials, skins, lights, cameras and animation clips.
`ModelLoader` and `UsdBackend` both produce one; `Renderer` consumes one. Adding a third
importer means adding one file.

**Skinning** runs in the vertex shader against a joint-matrix SSBO that the animator
refills each frame. Meshes without skins take the same path with an identity palette,
which keeps the pipeline count down.

