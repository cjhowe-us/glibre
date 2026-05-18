## Cursor Cloud specific instructions

### Overview

**glibre** is a no-code, cross-platform game engine. Currently the codebase contains a single
`Runtime` project — a C#/.NET 10.0 executable that opens an SDL3 window and renders a Vulkan
triangle demo. No editor or test projects exist yet.

### Build / Test / Lint

| Task | Command |
|---|---|
| Restore | `dotnet restore glibre.slnx` |
| Build | `dotnet build glibre.slnx -c Release` |
| Test | `dotnet test glibre.slnx -c Release` |
| Lint (markdown) | `rumdl check .` |

CI workflows (`.github/workflows/build.yml` and `lint.yml`) mirror these commands.

### Running the Runtime on Linux cloud VMs

The Runtime renders via Vulkan using the Lavapipe (Mesa) software rasterizer. To run:

```bash
export XDG_RUNTIME_DIR=/run/user/$(id -u)
mkdir -p $XDG_RUNTIME_DIR
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
dotnet run --project Runtime -c Release
```

`VK_ICD_FILENAMES` forces the Lavapipe software ICD (no GPU required).
`XDG_RUNTIME_DIR` must be set or SDL3 will print a warning.

On macOS, the surface is created via Metal (`vkCreateMetalSurfaceEXT`). On Linux/Windows, it
uses `SDL_Vulkan_CreateSurface` which selects the appropriate WSI (X11/Wayland).

### Environment notes

- .NET 10.0 SDK (preview) is required. Installed via `dotnet-install.sh --channel 10.0`.
- Rust stable toolchain is required. Managed via `rustup`.
- `rumdl` is installed via `cargo install rumdl`.
- Mesa Vulkan drivers (`mesa-vulkan-drivers`) provide the Lavapipe software Vulkan ICD.
- The `DOTNET_ROOT` is `/usr/local/share/dotnet`. Cargo binaries live in `/usr/local/cargo/bin`
  (both on PATH).
