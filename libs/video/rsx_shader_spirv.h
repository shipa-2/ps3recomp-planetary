/*
 * ps3recomp - HLSL -> SPIR-V (Vulkan rules), for the Plume/Vulkan RSX backend
 *
 * The RSX decompilers (rsx_vp_decompiler.c, rsx_fp_decompiler.c) emit HLSL,
 * the same source rsx_shader_msl.cpp feeds through glslang on its way to
 * MSL. This header exposes the intermediate SPIR-V that step already
 * produces, so a Vulkan backend (see libs/video/plume_renderer/) can hand it
 * straight to plume::RenderDevice::createShader() with
 * RenderShaderFormat::SPIRV -- no second decompiler, no spirv-cross
 * dependency, just the glslang half of the existing MSL pipeline.
 *
 * Binding contract: identical to rsx_shader_msl.h's, since it is the same
 * glslang HLSL front end with the same auto-mapped bindings/locations. The
 * SPIR-V entry point is always `main` (glslang's HLSL front end does not
 * rename it away, unlike spirv-cross's MSL output).
 */
#ifndef PS3RECOMP_RSX_SHADER_SPIRV_H
#define PS3RECOMP_RSX_SHADER_SPIRV_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_SHADER_STAGE_VERTEX   0
#define RSX_SHADER_STAGE_FRAGMENT 1

/* 1 when the translator was built with glslang, 0 when the stub is in. */
int rsx_hlsl_to_spirv_available(void);

/* Translate one HLSL shader whose entry point is `main` into SPIR-V words.
 *   hlsl        : NUL-terminated HLSL source, as the decompilers emit it.
 *   stage       : RSX_SHADER_STAGE_VERTEX or RSX_SHADER_STAGE_FRAGMENT.
 *   out_words   : receives the SPIR-V module, as 32-bit words.
 *   max_words   : capacity of out_words.
 *   out_count   : receives the word count actually written.
 *   log         : receives a diagnostic on failure (may be NULL).
 * Returns 0 on success, -1 on failure (bad input, a front-end error, an
 * output buffer too small, or the stub). */
int rsx_hlsl_to_spirv(const char* hlsl, int stage,
                      u32* out_words, u32 max_words, u32* out_count,
                      char* log, u32 log_size);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_SHADER_SPIRV_H */
