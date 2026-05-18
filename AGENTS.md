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

### Runtime limitations on Linux cloud VMs

The Runtime project creates a Vulkan surface via `SDL_Metal_CreateView` / `vkCreateMetalSurfaceEXT`,
which is macOS-only. On Linux, the app will build and start (SDL3 init + Vulkan instance creation
succeed), but will fail at surface creation with:

```
SDL_Metal_CreateView failed: That operation is not supported
```

This is expected — the codebase targets macOS/iOS first (see `CLAUDE.md`). Full graphical runtime
testing requires a macOS host with MoltenVK.

### Environment notes

- .NET 10.0 SDK (preview) is required. Installed via `dotnet-install.sh --channel 10.0`.
- `rumdl` is installed via `pip install rumdl` (binary wheel, no Rust/cargo needed).
- Mesa Vulkan drivers (`mesa-vulkan-drivers`) provide the Lavapipe software Vulkan ICD for
  headless Vulkan enumeration, but rendering is blocked by the Metal-only surface path.
- The `DOTNET_ROOT` and dotnet/rumdl binaries are on PATH via `/usr/local/share/dotnet` and
  `~/.local/bin` respectively.
