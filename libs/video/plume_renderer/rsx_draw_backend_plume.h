/*
 * ps3recomp - rsx_draw_backend implementation on top of Plume (Vulkan)
 *
 * This is the cross-platform answer to "no Vulkan backend, and the only
 * real rendering path is Win32+D3D12" (docs/RENDERER_MIGRATION.md): one
 * rsx_draw_backend implementation, driven by Plume's RHI, that runs on
 * Linux (and, later, wherever else Plume's Vulkan/Metal/D3D12 backends
 * reach) instead of a third hand-written platform backend.
 *
 * Phase 1 scope (see the .cpp for the exact split): a real SDL window, a
 * real Plume device/swapchain, and real color targets/clears/present.
 * pipeline_create() now compiles the decompilers' HLSL to SPIR-V (via
 * rsx_shader_spirv.h) and builds a real Plume graphics pipeline from it,
 * with blend/cull/front-face wired from the guest's own render state.
 * Textures, depth/stencil and draw() itself are not implemented yet:
 * nothing binds a pipeline or issues a draw call, so every draw remains a
 * no-op (dropped rather than mis-rendered) until bind_pipeline, the other
 * bind_ calls, and draw() are wired up next.
 */
#ifndef PS3RECOMP_RSX_DRAW_BACKEND_PLUME_H
#define PS3RECOMP_RSX_DRAW_BACKEND_PLUME_H

#include "rsx_draw_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the SDL2 window + Plume device/swapchain and fills *out with the
 * rsx_draw_backend vtable, ready for rsx_draw_engine_set_backend(). Returns
 * 0 on success, -1 on failure (SDL/Vulkan init failure; check stderr). */
int rsx_draw_backend_plume_create(rsx_draw_backend* out, const char* window_title);

/* Tears down everything rsx_draw_backend_plume_create made. Safe to call
 * with a backend whose init() was never invoked by the engine. */
void rsx_draw_backend_plume_destroy(rsx_draw_backend* backend);

/* Pumps SDL's event queue (resize/quit). Returns 0 if the window received a
 * quit request, 1 otherwise. A host's frame loop calls this once per frame;
 * the engine itself has no concept of a window event loop. */
int rsx_draw_backend_plume_pump_events(rsx_draw_backend* backend);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_DRAW_BACKEND_PLUME_H */
