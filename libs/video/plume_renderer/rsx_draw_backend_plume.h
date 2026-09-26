/*
 * ps3recomp - rsx_draw_backend implementation on top of Plume (Vulkan)
 *
 * This is the cross-platform answer to "no Vulkan backend, and the only
 * real rendering path is Win32+D3D12" (docs/RENDERER_MIGRATION.md): one
 * rsx_draw_backend implementation, driven by Plume's RHI, that runs on
 * Linux (and, later, wherever else Plume's Vulkan/Metal/D3D12 backends
 * reach) instead of a third hand-written platform backend.
 *
 * Phase 1 scope (see the .cpp for the exact split): a real SDL2 window, a
 * real Plume device/swapchain, and real color targets/clears/present.
 * Textures, depth and pipelines are not yet implemented -- draw() takes the
 * "no textures, no depth" fallback: a title's own vertex/fragment programs
 * are not run yet, and every draw call is dropped rather than mis-rendered.
 * That grows in the next phase once rsx_hlsl_to_spirv (rsx_shader_spirv.h)
 * is wired to pipeline_create.
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
