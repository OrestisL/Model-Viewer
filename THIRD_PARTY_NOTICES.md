# Third-party components

Every dependency is fetched from source at configure time; none is vendored in
this repository. All are permissively licensed and none imposes copyleft
obligations on binaries you distribute, but the notices below should ship with
any binary release.

| Component | Version | License | Purpose |
|---|---|---|---|
| [Assimp](https://github.com/assimp/assimp) | v5.4.3 | BSD-3-Clause | Model import and export |
| [Draco](https://github.com/google/draco) | vendored in Assimp | Apache-2.0 | glTF mesh decompression |
| [tinyusdz](https://github.com/lighttransport/tinyusdz) | `bd2a1ed` | Apache-2.0 / MIT | USD import (optional) |
| [GLFW](https://github.com/glfw/glfw) | 3.4 | Zlib | Windowing and input |
| [GLM](https://github.com/g-truc/glm) | 1.0.1 | MIT | Maths |
| [Dear ImGui](https://github.com/ocornut/imgui) | v1.91.8-docking | MIT | User interface |
| [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo) | `5ab7676` | MIT | Transform manipulator |
| [vk-bootstrap](https://github.com/charles-lunarg/vk-bootstrap) | v1.3.290 | MIT | Vulkan initialisation |
| [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | v3.1.0 | MIT | GPU memory allocation |
| [stb_image](https://github.com/nothings/stb) | `f056911` | MIT / Public domain | Texture decoding |

The Vulkan loader and headers come from the LunarG SDK or the distribution's
packages and are **not** redistributed with this application. Users supply
their own via their GPU driver.

To regenerate full licence texts for a release, the simplest route is to
collect `LICENSE` from each populated dependency under `build/*/\_deps/`.
