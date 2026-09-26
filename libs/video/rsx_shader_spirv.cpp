/*
 * ps3recomp - HLSL -> SPIR-V, through glslang
 *
 * See rsx_shader_spirv.h. This is deliberately the glslang half of
 * rsx_shader_msl.cpp's pipeline, lifted out on its own: a Vulkan backend
 * needs SPIR-V only, not the spirv-cross -> MSL leg, so it should not have
 * to link spirv-cross to get it. The HLSL -> SPIR-V step itself is
 * unchanged from rsx_shader_msl.cpp: Vulkan client rules, SPIR-V 1.3,
 * auto-mapped bindings/locations via TProgram::mapIO. See that file's
 * header comment for why each of those choices was made; nothing here
 * repeats them, since drift between the two would be exactly the
 * "second decompiler" problem both files exist to avoid.
 */
#include "rsx_shader_spirv.h"
#include <stdio.h>
#include <string.h>

#if defined(PS3RECOMP_HAVE_SPIRV_TRANSLATION)

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <vector>

namespace {

void put_log(char* log, u32 log_size, const char* what, const char* detail)
{
    if (!log || log_size == 0) return;
    snprintf(log, log_size, "%s%s%s", what ? what : "",
             (what && detail && *detail) ? ": " : "", detail ? detail : "");
}

bool glslang_ready()
{
    static bool inited = false;
    if (!inited) {
        if (!glslang::InitializeProcess()) return false;
        inited = true;
    }
    return true;
}

bool hlsl_to_spirv(const char* hlsl, EShLanguage lang, std::vector<unsigned int>& spirv,
                   char* log, u32 log_size)
{
    const EShMessages msgs = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl);

    glslang::TShader shader(lang);
    const char* strings[1] = { hlsl };
    shader.setStrings(strings, 1);
    shader.setEnvInput(glslang::EShSourceHlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    shader.setEntryPoint("main");
    shader.setAutoMapBindings(true);
    shader.setAutoMapLocations(true);
    if (!shader.parse(GetDefaultResources(), 100, false, msgs)) {
        put_log(log, log_size, "HLSL parse", shader.getInfoLog());
        return false;
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msgs)) {
        put_log(log, log_size, "HLSL link", program.getInfoLog());
        return false;
    }
    if (!program.mapIO()) {
        put_log(log, log_size, "HLSL io map", program.getInfoLog());
        return false;
    }

    spv::SpvBuildLogger logger;
    glslang::SpvOptions opts;
    opts.disableOptimizer = false;
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &logger, &opts);
    if (spirv.empty()) {
        put_log(log, log_size, "SPIR-V generation", logger.getAllMessages().c_str());
        return false;
    }
    return true;
}

} // namespace

extern "C" int rsx_hlsl_to_spirv_available(void) { return 1; }

extern "C" int rsx_hlsl_to_spirv(const char* hlsl, int stage,
                                 u32* out_words, u32 max_words, u32* out_count,
                                 char* log, u32 log_size)
{
    if (log && log_size) log[0] = '\0';
    if (out_count) *out_count = 0;
    if (!hlsl || !out_words || max_words == 0 || !out_count) {
        put_log(log, log_size, "bad arguments", nullptr);
        return -1;
    }
    if (!glslang_ready()) {
        put_log(log, log_size, "glslang::InitializeProcess failed", nullptr);
        return -1;
    }
    const EShLanguage lang = (stage == RSX_SHADER_STAGE_VERTEX) ? EShLangVertex : EShLangFragment;
    std::vector<unsigned int> spirv;
    if (!hlsl_to_spirv(hlsl, lang, spirv, log, log_size)) return -1;
    if (spirv.size() > max_words) {
        put_log(log, log_size, "SPIR-V output buffer too small", nullptr);
        return -1;
    }
    memcpy(out_words, spirv.data(), spirv.size() * sizeof(unsigned int));
    *out_count = (u32)spirv.size();
    return 0;
}

#else /* !PS3RECOMP_HAVE_SPIRV_TRANSLATION */

extern "C" int rsx_hlsl_to_spirv_available(void) { return 0; }

extern "C" int rsx_hlsl_to_spirv(const char* hlsl, int stage,
                                 u32* out_words, u32 max_words, u32* out_count,
                                 char* log, u32 log_size)
{
    (void)hlsl; (void)stage; (void)out_words; (void)max_words;
    if (out_count) *out_count = 0;
    if (log && log_size)
        snprintf(log, log_size, "HLSL to SPIR-V translation was not built "
                                "(glslang not found at configure time)");
    return -1;
}

#endif
