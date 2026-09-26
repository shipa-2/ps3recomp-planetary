# Renderer migration: retiring the Windows-only path, adding Vulkan via Plume

## Why

Before this branch, the only rendering path that actually runs a title
(`libs/video/rsx_live_draw.c`) is Win32 + D3D12 only (`#if !defined(_WIN32)`
compiles the whole file to no-ops). Metal exists but lags in shader/texture
completeness. There is no Vulkan backend at all, so Linux has never had real
GPU rendering (only the headless software null backend).

Separately, `libs/video/rsx_draw_engine.h` already defines a platform-neutral
engine: it owns state tracking, vertex compaction, texture layout and the
NV40 VP/FP -> HLSL decompilers, and drives a backend through the narrow
`rsx_draw_backend` interface (texture/target/pipeline creation, bind, draw,
clear, present). Metal already implements this interface
(`rsx_metal_backend.m`'s `s_engine_backend`); D3D12 does not yet.

Rather than hand-write a third backend (D3D12 vtable-only, Metal vtable +
engine, and now a from-scratch Vulkan one), this branch adds **one**
`rsx_draw_backend` implementation on top of [Plume](https://github.com/renderbag/plume),
an MIT-licensed RHI over Vulkan/D3D12/Metal. Plume is not a new dependency
picked for this project alone: it already backs a real, shipping Xenos
(Xbox 360) recompilation project's GPU plugin (`rexgpu-plume`), which is
where the design below (offscreen color target + copy-to-swapchain-at-present,
SDL2 windowing, HLSL -> SPIR-V via glslang feeding `createShader` directly)
is carried over from.

## Which Plume checkout this needs

`PLUME_ROOT` (`../plume` next to this repo) must be
[`shipa-2/plume`](https://github.com/shipa-2/plume), not upstream
[`renderbag/plume`](https://github.com/renderbag/plume): the fork links SDL3
unconditionally (`find_package(SDL3 REQUIRED)`, no SDL2 fallback), and
`rsx_draw_backend_plume.cpp` is written against the SDL3 API to match.
Mixing SDL2 and SDL3 headers in one translation unit is an ODR pileup
(`SDL_bool`, `SDL_ScaleMode`, `SDL_CreateWindow`'s own argument count all
differ) that no include-path trick fixes -- if you see a wall of `SDL_bool`/
`SDL_ScaleMode` redefinition errors, you have the wrong Plume checkout, not
a build environment problem.

Separately, and for a different reason: `ps3recomp_rsx_plume` is **not**
linked into `ps3recomp_runtime`, and cannot be until it becomes a plugin.
`ps3recomp_runtime` links `SDL2::SDL2` itself (cellPad/cellAudio), and
CMake's own `SDL2::SDL2` imported target carries a
`COMPATIBLE_INTERFACE_STRING SDL_VERSION` property -- having both that and
an SDL3 target as transitive dependencies of the same final target is a
hard CMake generate-time error, not a linker warning. This is exactly why
Xerenge's own Plume backend (`rexgpu-plume`) is a separate *shared* library
loaded as a plugin rather than statically linked into the host executable.
Until this backend gets the same treatment, a project that wants both
cellPad/cellAudio and this Vulkan backend in one process needs that split
first -- see "Next steps" below.

## What Plume does and does not solve

Plume abstracts *device/resource/command* differences between Vulkan, D3D12
and Metal. It is explicitly "bring your own compiler": it does not
cross-compile shaders. That part was already half-solved in this repo --
`rsx_shader_msl.cpp` already runs the RSX decompilers' HLSL output through
glslang to get SPIR-V, then spirv-cross to get MSL (for the Metal backend).
This branch adds `rsx_shader_spirv.{h,cpp}`, which is exactly the glslang
half of that pipeline exposed on its own (no spirv-cross dependency): HLSL
in, SPIR-V words out, ready for `plume::RenderDevice::createShader(...,
RenderShaderFormat::SPIRV)`. No second shader compiler was written.

## Status of this branch (Phase 1)

Real and working:
- `libs/video/plume_renderer/rsx_draw_backend_plume.{h,cpp}`: an
  `rsx_draw_backend` implementation using a real SDL2 window and a real
  Plume Vulkan device/queue/swapchain.
- `init`/`shutdown`/`submit_and_wait`: real.
- `color_target_create`/`color_target_release`: real (an offscreen
  `plume::RenderTexture` + single-color-attachment framebuffer per surface).
- `clear_color`: real.
- `present`: real -- copies the offscreen surface into the acquired
  swapchain image and presents it. Assumes the surface and the swapchain are
  the same size (true for a display buffer created at the window's own
  resolution); a mismatched size needs `copyTextureRegion` instead of the
  current whole-texture `copyTexture`.
- `rsx_shader_spirv.{h,cpp}`: real, and validated independently of the
  backend -- see "What was actually verified" below.
- CMake: builds whenever a Plume checkout exists at `../plume` next to this
  repository (same sibling-directory convention as Xerenge's own
  `rexgpu-plume`), gated by `PS3RECOMP_RSX_BACKEND_PLUME` (default ON,
  non-Apple Unix). `PLUME_SDL_VULKAN_ENABLED` is forced on for this backend.

Explicit stubs (return "not built" / no-op, matching the engine's contract
that a 0 handle means "could not build" and is safely cached):
- `texture_create`/`texture_upload`/`texture_release`
- `depth_target_create`/`depth_target_release`/`depth_snapshot`
- `pipeline_create`/`pipeline_release`
- every `bind_*`, `set_viewport`, `set_scissor`, `set_stencil_ref`
- `draw` (a no-op: frames clear and present correctly, but carry no guest
  geometry yet)
- `readback`

This means: real window, real Vulkan device, real per-frame clear+present
through Plume -- and no guest draws yet. That is the same "phase 1" scope
Xerenge's own Plume backend started from (see its `plume_swapchain.h`:
"Minimal plume swapchain: clear color + present (phase 1)").

## What was actually verified in this environment

This sandbox has no GPU and no real display, but does have Mesa's software
Vulkan driver (lavapipe) and Xvfb -- and, per the repo owner, is not where
graphical tests actually get run (that happens on their own machine with
real hardware). So verification here is deliberately scoped to *builds*,
not to running/watching a frame get presented:

- A real SDL3 (3.2.31, built from source with Vulkan enabled) and the
  actual `shipa-2/plume` fork (not upstream) were used -- not a stand-in.
- `rsx_shader_spirv.cpp`, the `plume` static library, `ps3recomp_rsx_plume`,
  and `ps3recomp_plume_smoke` all configure and build cleanly against them,
  with no ODR conflicts and no CMake generate-time errors.
- Earlier attempts (see git history around commits 6f77d11, 7ec929d, and
  the fix in 6364460) mis-cloned upstream `renderbag/plume` (SDL2) instead
  of `shipa-2/plume` (SDL3) and chased what looked like an include-order bug
  for two rounds before the real cause -- the wrong Plume checkout, plus a
  real CMake linking constraint -- was found. If a future build breaks the
  same way, check which Plume checkout is at `../plume` first.

**Confirmed on the repo owner's own hardware** (AMD/RADV, Wayland): after
fixing the semaphore bug below, `ps3recomp_plume_smoke` presents 180 real
frames end-to-end with no hang -- Phase 1 (real window, real Plume Vulkan
device/queue/swapchain, real color-target clear+present) is verified
working, not just building.

One bug found and fixed this way: `Present()` built a per-frame swapchain
release semaphore and told `swap_chain_->present()` to wait on it, but the
preceding `executeCommandLists()` submit signaled nothing -- so `present()`
waited forever on a semaphore nothing was ever going to signal. A gdb
backtrace on the repo owner's machine (main thread stuck in
`plume::VulkanSwapChain::present()`'s DRM syncobj wait) pinned this down
immediately; guessing from this sandbox alone would not have found it, since
lavapipe's software presentation path didn't reproduce the hang. Fixed by
having the submit signal that semaphore (and wait on the acquire semaphore,
which also wasn't being waited on before the GPU work touched the acquired
image).

## Next steps (Phase 2)

1. ~~Confirm on real hardware that `ps3recomp_plume_smoke` presents a
   visible, correctly-colored frame end to end~~ -- done, see above.
1b. Give `ps3recomp_rsx_plume` the same plugin split Xerenge's `rexgpu-plume`
   has (a separate shared library, loaded at runtime rather than statically
   linked) so a game can use it alongside `ps3recomp_runtime`'s own SDL2
   usage (cellPad/cellAudio) in one process -- right now they cannot coexist
   in the same statically-linked binary (see "Which Plume checkout this
   needs" above).
2. `pipeline_create`: call `rsx_hlsl_to_spirv()` for both stages, then
   `device_->createShader(..., RenderShaderFormat::SPIRV)`, and build a
   `RenderGraphicsPipelineDesc` from `rsx_be_render_state` +
   `rsx_vertex_layout_plan` (blend/depth/cull descriptors, input layout).
   `examples/triangle/main.cpp` in the Plume checkout shows the exact shape
   of every call needed.
3. `draw`: wire vertex/index data through to `setVertexBuffers` +
   `drawInstanced`/`drawIndexedInstanced`.
4. `texture_create`/`texture_upload`: real `RenderTexture` + staging-buffer
   upload.
5. `depth_target_create`/`clear_depth_stencil`: a depth `RenderTexture`
   alongside each color target's framebuffer.
6. `color_target_create`'s `seed` parameter (uploading the guest's own
   CPU-initialised bytes for a surface a title samples before drawing into
   it) is currently ignored -- wire it through a staging buffer.
7. Once parity is reasonable, retire `rsx_d3d12_backend.c`'s direct-vtable
   path and `rsx_live_draw.c`'s monolithic D3D12 usage in favor of this
   engine, the way Metal already did -- so Windows, Linux and macOS share
   one engine and only differ in which Plume backend they select.
