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

This container has no GPU but does have Mesa's software Vulkan driver
(lavapipe) and Xvfb, which is enough to validate the toolchain without real
hardware:

- Plume's own `examples/triangle` was built and run under Xvfb + lavapipe:
  a real Vulkan device was created, and over 2000 frames were rendered and
  presented in a few seconds. This confirms Plume + SDL2 + Vulkan + lavapipe
  works end-to-end in this kind of environment.
- `ps3recomp_runtime` (the full static library, including
  `rsx_shader_spirv.cpp` and `rsx_draw_backend_plume.cpp`) configures and
  builds cleanly with `PS3RECOMP_RSX_BACKEND_PLUME` on.
- The `ps3recomp_plume_smoke` executable (see
  `libs/video/tests/test_plume_backend_smoke.c`) builds cleanly.

What was **not** confirmed in this environment: `ps3recomp_plume_smoke`
actually presenting a visible frame end-to-end. It was left hanging with no
output under Xvfb + lavapipe when this branch was prepared, and that was not
root-caused before handing off -- it may be a real bug in
`rsx_draw_backend_plume.cpp` (something in `Init`/`ColorTargetCreate`/
`Present` blocking), or it may be an Xvfb/lavapipe environment quirk (the
Plume triangle example was tested with its own single-shot run, not
repeatedly in the same container session as the ps3recomp binary). **Treat
Phase 1 as unverified end-to-end until it has been run on a real GPU/display
and confirmed to present visible frames.**

## Next steps (Phase 2)

1. Root-cause and fix whatever is blocking `ps3recomp_plume_smoke` (start by
   running it under `vulkaninfo`/`VK_LOADER_DEBUG=all` and with a debugger
   attached, or by testing `Init`/`ColorTargetCreate`/`Present` in isolation).
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
