# AGENTS.md — dxvk-remix-nv

This file provides context for AI coding agents working in this repository.

> ## Read `CLAUDE.md` first — this is not stock dxvk-remix
>
> Everything below is upstream NVIDIA's build, style and layout guidance, and it
> is still correct. What it does **not** carry is why this fork exists, and an
> agent working from this file alone will get the design intent wrong.
>
> **`CLAUDE.md` at the repo root is the authority** on all of the following, and
> none of it is repeated here:
>
> - **What the D3D9 renderer is for.** This fork serves one game, Dusklight,
>   rendering through a GX→D3D9 fixed-function backend. **The raw fixed-function
>   D3D9 image is never shown to a player** — it is the feed into Remix's
>   renderer, which is the product. So fixed-function limits are not a ceiling:
>   where the D3D9 stream cannot carry something, the answer is a change *here*,
>   in the fork or through the Remix API, not a contortion of the D3D9 stream.
>   "Raw D3D9 stays correct" is a safety property, never a design goal. The two
>   exceptions that must still rasterize correctly are the **HUD** (Remix
>   rasterizes UI draws) and **alpha** (Remix reads it for opacity and the alpha
>   test). Canonical statement:
>   `aurora-ao/docs/dx9/remix-material-interface.md` §0.
> - **The game/DLL protocol.** `rtx.dusklight.env.protocol` vs
>   `kRequiredProtocol`; skew has cost an evening twice.
> - **Branch rules** — push only your session branch, and the containment check
>   before any branch is deleted.
> - **Two tripwires that only fire in CI**, `CheckRtInstanceSize` and
>   `hashStructByMemory`, neither of which shows up in a Linux container.
> - The five project rules, including *a fix that cannot be observed is a guess*
>   and *say what was verified and what was not*.
> - **The game's symbols are romanized Japanese.** The naming rules below govern
>   *this repo* and are unaffected — but `kankyo` (環境) is *environment*,
>   `wether` is the game's spelling of *weather*, and the game tree spells some
>   words two ways, so a search for one spelling finds half a feature. Reference:
>   `dusklight-ao/docs/japanese-naming.md`.
>
> Design documents for this fork's own work:
> `documentation/DusklightAtmosphere.md` (the rendering, and §11 the whole-fork
> rebase surface) and `documentation/DusklightOverlay.md` (the control plane).

## Project Overview

dxvk-remix is a fork of DXVK that overhauls the fixed-function D3D9 graphics pipeline for path-traced remastering of classic games. The `bridge` subfolder enables 32-bit games to communicate with the 64-bit runtime.

Only x64 build targets are supported.

> Dusklight is 64-bit and loads `d3d9.dll` directly, so it never uses the
> `bridge`; the x86 bridge steps were removed from this fork's CI on 2026-07-28.

## Shell

Commands are run in **PowerShell**. Use `;` to chain commands (not `&&`).

## Procedural Skills

Step-by-step workflows for common tasks are in `.agents/skills/`:

| Skill | Description |
|-------|-------------|
| `build-remix` | Build the project (first-time setup, incremental builds, reconfigure) |
| `deploy-and-test-game` | Deploy to a game target, launch, and debug |
| `run-unit-tests` | Build and run unit tests |

Some additional test instructions are in `tests/rtx/dxvk_rt_testing/AGENTS.md` (private NVIDIA GitLab only).

## C++ Coding Standards

Full guide: `documentation/CONTRIBUTING-style-guide.md`

### Key Rules

- **Indentation**: 2 spaces, no tabs.
- **Braces**: Always use braces for `if`, `else`, `for`, `while`, etc. Opening brace on the same line.
- **Naming**:
  - Member variables: `m_` prefix (e.g. `m_value`)
  - Pointers: `p` prefix (e.g. `pInput`, `m_pPointer`)
  - Variables and functions: `camelCase`
  - Functions: Prefer short verb + object names aligned with the subsystem; avoid encoding implementation steps in the identifier (see `documentation/CONTRIBUTING-style-guide.md`, Naming Conventions).
  - Constants: `k` prefix and camelCase, i.e. `kConstantName`
  - Macros and defines: `UPPER_CASE`
  - Classes and structs: `PascalCase`
  - **These apply to code written here.** Game symbols quoted in comments or
    documentation keep the game's own spelling — they are romanized Japanese and
    must not be Anglicised (`dusklight-ao/docs/japanese-naming.md`).
- **Includes**: Standard library first, then third-party, then local. Separate groups with blank lines.
- **Memory**: Prefer smart pointers (`std::unique_ptr`, `std::shared_ptr`). Use `Rc<T>` for GPU resources.
- **Profiling**: Use `ScopedCpuProfileZone()` / `ScopedGpuProfileZone(ctx, "name")` for performance-critical code.

### Changes to Core DXVK Files (Applies to code files outside of `rtx_render`)

Wrap diverging code in comment blocks:

```cpp
// NV-DXVK start: Brief description of change
// ... changed code ...
// NV-DXVK end
```

### Adding New Source Files

- New `.cpp` and `.h` files must be added to the `dxvk_src` list in `src/dxvk/meson.build` (alphabetically, both `.cpp` and `.h` on adjacent lines).
- New shader files (`.comp.slang`, `.rgen.slang`, etc.) are auto-discovered from `src/dxvk/shaders/rtx/` — no build system registration needed.


## RTX Options

- Add new options in the relevant feature class, not in `rtx_options.h`.
- Use categorized string names: `RTX_OPTION("rtx.category", type, name, default, "Description.")`.
- Use `RTX_ENV_VAR` (not `DXVK_ENV_VAR`) for RTX-related environment variables.
- Enumerate enum values in descriptions: `0: First, 1: Second, ...`.
- Regenerate `RtxOptions.md` by running with `DXVK_DOCUMENTATION_WRITE_RTX_OPTIONS_MD=1`.

## Shader Code

Full guide: `src/dxvk/shaders/rtx/README.md`

Shaders use [Slang](https://github.com/shader-slang/slang) (GLSL-compatible). All RTX shaders live in `src/dxvk/shaders/rtx/`.

### File Extensions

| Extension | Purpose |
|-----------|---------|
| `*.h` | Structure definitions shared across files (and sometimes with CPU) |
| `*.slangh` | Implementations and helpers (this is what other files include) |
| `*.comp.slang` | Compute shaders |
| `*.rgen.slang` | Ray generation shaders |
| `*.rchit.slang` | Ray closest hit shaders |
| `*.rahit.slang` | Ray any hit shaders |
| `*.rmiss.slang` | Ray miss shaders |

### Naming

Files and folders use `lower_snake_case`, named after the primary type or functionality they provide.

### Shared C++/Shader Headers

Files in `src/dxvk/shaders/rtx/` with `.h` extension are shared between C++ and Slang via `#ifdef __cplusplus` guards. Key examples:
- `rtx/pass/instance_data.h` — `InstanceData` struct (per-TLAS-instance data)
- `rtx/utility/shader_types.h` — `vec4`, `vec3`, `vec2`, `uint` types compatible in both C++ and Slang
- `rtx/pass/common_binding_indices.h` — Binding index constants

When modifying shared headers, ensure both C++ and Slang code paths remain consistent. GPU struct size constants (e.g. `kSurfaceGPUSize`, `INSTANCE_DATA_GPU_SIZE`) must match their respective struct sizes.

### Slang Conventions

- `mat4x3` for 3x4 matrices (3 rows of 4 columns, row-major in Slang).
- `f16vec3` / `float16_t` for half-precision where appropriate.
- `BUFFER_ARRAY(bufferName, bufferIndex, elementIndex)` macro for bindless buffer access.
- `BINDING_INDEX_INVALID` sentinel for missing buffer bindings.
- Struct properties use getter/setter patterns with bitfield packing (see `surface.h`).

### C++ GPU Conventions

- `Matrix4` for 4x4 matrices, `Vector4` for 4-component vectors.
- `vec4` from `shader_types.h` is `alignas(16)` — 16 bytes, compatible with GPU layout.
- GPU buffer writes use `memcpy` for matrix rows or direct `vec4` assignment.

## Pull Requests

- Squash into a single commit before submitting.
- Limit changes to those required for the PR goal — no drive-by style fixes.
- Add your name to `src/dxvk/imgui/dxvk_imgui_about.cpp` under "GitHub Contributors" (A-Z by last name).


## Key Directories

| Path | Description |
|------|-------------|
| `src/dxvk/rtx_render/` | Core RTX rendering code |
| `src/dxvk/shaders/rtx/` | RTX shader code (Slang) |
| `src/dxvk/imgui/` | ImGui integration and developer UI |
| `src/util/` | Shared utility code |
| `bridge/` | 32-bit to 64-bit bridge |
| `tests/rtx/unit/` | Unit tests |
| `documentation/` | Project documentation |
