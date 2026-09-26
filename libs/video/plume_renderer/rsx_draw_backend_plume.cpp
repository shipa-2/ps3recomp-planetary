/*
 * ps3recomp - rsx_draw_backend implementation on top of Plume (Vulkan)
 *
 * See the header for scope. This file follows the same shape Xerenge's own
 * rexgpu-plume backend uses for its Xenos target (plume_swapchain.cpp /
 * plume_graphics_system.cpp): an SDL window, plume::CreateVulkanInterface,
 * one RenderDevice/RenderCommandQueue/RenderCommandList/RenderCommandFence,
 * and offscreen RenderTexture(s) that get copied into the swapchain image at
 * present time rather than being rendered into the swapchain image
 * directly -- so a title's own render-to-texture and multiple simultaneous
 * surfaces (a title can hold more than one cellGcm display buffer alive at
 * once) both just work, with the swapchain only ever seeing the one surface
 * a flip actually presents.
 *
 * Written against SDL3 (SDL_Init/SDL_PollEvent return bool, SDL_CreateWindow
 * takes no x/y, the quit event is SDL_EVENT_QUIT) to match the Plume
 * checkout this project builds against (see CMakeLists.txt's PLUME_ROOT
 * comment: shipa-2/plume, which links SDL3 unconditionally). Do not include
 * <SDL.h>/SDL2 headers anywhere in this file: plume_render_interface_types.h
 * already pulls in <SDL3/SDL_vulkan.h>, and mixing SDL2 and SDL3 headers in
 * one translation unit is an ODR pileup (SDL_bool, SDL_ScaleMode, etc.),
 * not something fixable by include order.
 */
#include "rsx_draw_backend_plume.h"
#include "rsx_shader_spirv.h"

#include <plume_render_interface.h>

#include <SDL3/SDL.h>

#include <cstdio>
#include <memory>
#include <unordered_map>
#include <vector>

namespace plume {
// This Plume checkout's CreateVulkanInterface() is always niladic: the
// window is not needed until swapchain creation (RenderSwapChainDesc takes
// it there instead). See plume_vulkan.cpp's single definition.
extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
} // namespace plume

namespace {

using namespace plume;

constexpr uint32_t kSwapchainImageCount = 2;
constexpr RenderFormat kSwapchainFormat = RenderFormat::B8G8R8A8_UNORM;

RenderFormat ToPlumeFormat(rsx_be_format fmt)
{
    switch (fmt) {
        case RSX_BE_FMT_R8:               return RenderFormat::R8_UNORM;
        case RSX_BE_FMT_R8G8:             return RenderFormat::R8G8_UNORM;
        case RSX_BE_FMT_R8G8B8A8:         return RenderFormat::R8G8B8A8_UNORM;
        case RSX_BE_FMT_R16:              return RenderFormat::R16_UNORM;
        case RSX_BE_FMT_R16G16:           return RenderFormat::R16G16_UNORM;
        case RSX_BE_FMT_R16G16F:          return RenderFormat::R16G16_FLOAT;
        case RSX_BE_FMT_R16G16B16A16F:    return RenderFormat::R16G16B16A16_FLOAT;
        case RSX_BE_FMT_R32F:             return RenderFormat::R32_FLOAT;
        case RSX_BE_FMT_R32G32B32A32F:    return RenderFormat::R32G32B32A32_FLOAT;
        /* BC1/2/3 are compressed texture formats (rsx_draw_backend's
         * texture_* calls, not a color target); Phase 1 does not create
         * textures yet, so these never reach here in practice. Map them
         * anyway so a future caller gets a real format instead of UNKNOWN. */
        case RSX_BE_FMT_BC1:              return RenderFormat::BC1_UNORM;
        case RSX_BE_FMT_BC2:              return RenderFormat::BC2_UNORM;
        case RSX_BE_FMT_BC3:              return RenderFormat::BC3_UNORM;
        default:                          return RenderFormat::R8G8B8A8_UNORM;
    }
}

/* rsx_be_render_state's fields carry the guest's own NV4097 register values
 * undecoded (see rsx_draw_engine.h's comment on rsx_be_render_state: "the
 * D3D12 enums left undecoded... each backend has its own table"). Those
 * values are GL-compatible enums, as rsx_d3d12_backend.c's own blend-factor
 * table documents ("RSX blend factors/equation carry GL enums") and its
 * rsx_cull_key function's CULL_FACE/FRONT_FACE constants confirm (0x0404
 * FRONT, 0x0405 BACK, 0x0408 FRONT_AND_BACK, 0x0900 CW, 0x0901 CCW). The
 * comparison-function and stencil-op tables below are the parallel
 * GL_NEVER..GL_ALWAYS (0x0200-0x0207) and GL_ZERO/GL_KEEP.. (0x1E00 range,
 * plus GL_INVERT 0x150A and the _WRAP variants 0x8507/0x8508) constants from
 * the same NV40-derived, OpenGL-register-compatible hardware family -- kept
 * here rather than re-deriving them, and cross-checked against the blend
 * table's already-verified magic numbers for the same guest fields.
 */

RenderComparisonFunction GlCompareToPlume(u32 f)
{
    switch (f) {
        case 0x0200: return RenderComparisonFunction::NEVER;
        case 0x0201: return RenderComparisonFunction::LESS;
        case 0x0202: return RenderComparisonFunction::EQUAL;
        case 0x0203: return RenderComparisonFunction::LESS_EQUAL;
        case 0x0204: return RenderComparisonFunction::GREATER;
        case 0x0205: return RenderComparisonFunction::NOT_EQUAL;
        case 0x0206: return RenderComparisonFunction::GREATER_EQUAL;
        case 0x0207: return RenderComparisonFunction::ALWAYS;
        default:     return RenderComparisonFunction::ALWAYS;
    }
}

RenderStencilOp GlStencilOpToPlume(u32 op)
{
    switch (op) {
        case 0x0000: return RenderStencilOp::ZERO;
        case 0x1E00: return RenderStencilOp::KEEP;
        case 0x1E01: return RenderStencilOp::REPLACE;
        case 0x1E02: return RenderStencilOp::INCREMENT_AND_CLAMP;
        case 0x1E03: return RenderStencilOp::DECREMENT_AND_CLAMP;
        case 0x150A: return RenderStencilOp::INVERT;
        case 0x8507: return RenderStencilOp::INCREMENT_AND_WRAP;
        case 0x8508: return RenderStencilOp::DECREMENT_AND_WRAP;
        default:     return RenderStencilOp::KEEP;
    }
}

/* Same table as rsx_d3d12_backend.c's gl_blend_factor_d3d/gl_blend_op_d3d,
 * retargeted to Plume's enum names -- see the comment above this block. */
RenderBlend GlBlendFactorToPlume(u32 f, bool is_alpha)
{
    switch (f & 0xFFFFu) {
        case 0x0000: return RenderBlend::ZERO;
        case 0x0001: return RenderBlend::ONE;
        case 0x0300: return is_alpha ? RenderBlend::SRC_ALPHA     : RenderBlend::SRC_COLOR;
        case 0x0301: return is_alpha ? RenderBlend::INV_SRC_ALPHA : RenderBlend::INV_SRC_COLOR;
        case 0x0302: return RenderBlend::SRC_ALPHA;
        case 0x0303: return RenderBlend::INV_SRC_ALPHA;
        case 0x0304: return RenderBlend::DEST_ALPHA;
        case 0x0305: return RenderBlend::INV_DEST_ALPHA;
        case 0x0306: return is_alpha ? RenderBlend::DEST_ALPHA     : RenderBlend::DEST_COLOR;
        case 0x0307: return is_alpha ? RenderBlend::INV_DEST_ALPHA : RenderBlend::INV_DEST_COLOR;
        case 0x0308: return RenderBlend::SRC_ALPHA_SAT;
        case 0x8001: return RenderBlend::BLEND_FACTOR;
        case 0x8002: return RenderBlend::INV_BLEND_FACTOR;
        case 0x8003: return RenderBlend::BLEND_FACTOR;
        case 0x8004: return RenderBlend::INV_BLEND_FACTOR;
        default:     return RenderBlend::ONE;
    }
}

RenderBlendOperation GlBlendOpToPlume(u32 e)
{
    switch (e & 0xFFFFu) {
        case 0x8007: return RenderBlendOperation::MIN;
        case 0x8008: return RenderBlendOperation::MAX;
        case 0x800A: return RenderBlendOperation::SUBTRACT;
        case 0x800B: return RenderBlendOperation::REV_SUBTRACT;
        default:     return RenderBlendOperation::ADD;   /* 0x8006 FUNC_ADD / unset */
    }
}

/* register(bN) -> constant buffer binding N, register(tN)/(sN) -> texture/
 * sampler binding N: the same contract rsx_shader_msl.h documents for the
 * MSL side (same glslang HLSL front end, same auto-mapped bindings). Fixed
 * regardless of any one pipeline's actual shader content, so one pipeline
 * layout is created once and reused for every pipeline. */
constexpr uint32_t kVsConstantsBinding = 0;
constexpr uint32_t kPsConstantsBinding = 1;

struct Surface {
    std::unique_ptr<RenderTexture> texture;
    std::unique_ptr<RenderFramebuffer> framebuffer;
    uint32_t width = 0;
    uint32_t height = 0;
    RenderFormat format = RenderFormat::UNKNOWN;
    /* Whether the texture is currently in COLOR_WRITE layout (true) or
     * COPY_SOURCE/UNKNOWN (false). Tracked so present() only inserts the
     * barrier it actually needs. */
    bool color_write = false;
};

class PlumeRsxBackend {
public:
    int Init(uint32_t width, uint32_t height, const char* title);
    void Shutdown();
    void SubmitAndWait(uint32_t reason);

    uint32_t ColorTargetCreate(rsx_be_format fmt, uint32_t w, uint32_t h,
                               const void* seed, uint32_t seed_row_bytes);
    void ColorTargetRelease(uint32_t surface);

    void ClearColor(uint32_t surface, const float rgba[4]);
    void Present(uint32_t surface);
    void Readback(uint32_t surface, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  void* out, uint32_t out_pitch);

    uint32_t PipelineCreate(const char* vs_hlsl, const char* ps_hlsl,
                            const rsx_be_render_state* rs,
                            const rsx_vertex_layout_plan* layout,
                            uint32_t vertex_stride, rsx_be_format rt_fmt, uint32_t rt_count);
    void PipelineRelease(uint32_t pipeline);

    int PumpEvents();

    SDL_Window* window() const { return window_; }

private:
    void EnsureListOpen();
    void EnsureListClosedAndSubmitted(RenderCommandSemaphore* wait_sem = nullptr,
                                      RenderCommandSemaphore* signal_sem = nullptr);
    Surface* FindSurface(uint32_t id);
    bool EnsurePipelineLayout();
    bool CompileHlslToSpirv(const char* hlsl, int stage, std::vector<uint32_t>& out_words);

    SDL_Window* window_ = nullptr;
    std::unique_ptr<RenderInterface> interface_;
    std::unique_ptr<RenderDevice> device_;
    std::unique_ptr<RenderCommandQueue> queue_;
    std::unique_ptr<RenderCommandList> list_;
    std::unique_ptr<RenderCommandFence> fence_;
    std::unique_ptr<RenderSwapChain> swap_chain_;
    std::unique_ptr<RenderCommandSemaphore> acquire_sem_;
    std::vector<std::unique_ptr<RenderCommandSemaphore>> release_sems_;

    std::unordered_map<uint32_t, Surface> surfaces_;
    uint32_t next_surface_id_ = 1;

    struct Pipeline {
        std::unique_ptr<RenderShader> vertex_shader;
        std::unique_ptr<RenderShader> pixel_shader;
        std::unique_ptr<RenderPipeline> pipeline;
    };
    std::unique_ptr<RenderPipelineLayout> pipeline_layout_;
    std::unordered_map<uint32_t, Pipeline> pipelines_;
    uint32_t next_pipeline_id_ = 1;

    bool list_open_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

Surface* PlumeRsxBackend::FindSurface(uint32_t id)
{
    auto it = surfaces_.find(id);
    return (it != surfaces_.end()) ? &it->second : nullptr;
}

int PlumeRsxBackend::Init(uint32_t width, uint32_t height, const char* title)
{
    /* SDL3's SDL_Init is additive/ref-counted, so calling it unconditionally
     * (unlike SDL2's own SDL_WasInit-guarded pattern) is safe even if the
     * host process already initialised SDL_INIT_VIDEO itself. */
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "[plume backend] SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    const uint32_t sdl_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN;
    window_ = SDL_CreateWindow(title ? title : "ps3recomp", (int)width, (int)height, sdl_flags);
    if (!window_) {
        std::fprintf(stderr, "[plume backend] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }
    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    interface_ = plume::CreateVulkanInterface();
    if (!interface_) {
        std::fprintf(stderr, "[plume backend] CreateVulkanInterface failed\n");
        return -1;
    }

    device_ = interface_->createDevice();
    if (!device_) {
        std::fprintf(stderr, "[plume backend] createDevice failed (no usable Vulkan device)\n");
        return -1;
    }

    queue_ = device_->createCommandQueue(RenderCommandListType::DIRECT);
    fence_ = device_->createCommandFence();
    list_ = queue_->createCommandList();
    acquire_sem_ = device_->createCommandSemaphore();

    RenderWindow render_window = window_;
    swap_chain_ = queue_->createSwapChain(RenderSwapChainDesc(render_window, kSwapchainFormat, kSwapchainImageCount));
    if (!swap_chain_ || !swap_chain_->resize()) {
        std::fprintf(stderr, "[plume backend] swap chain creation/resize failed\n");
        return -1;
    }

    width_ = width;
    height_ = height;
    return 0;
}

void PlumeRsxBackend::Shutdown()
{
    if (queue_ && fence_) {
        queue_->waitForCommandFence(fence_.get());
    }
    surfaces_.clear();
    release_sems_.clear();
    acquire_sem_.reset();
    swap_chain_.reset();
    list_.reset();
    fence_.reset();
    queue_.reset();
    device_.reset();
    interface_.reset();
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
}

void PlumeRsxBackend::EnsureListOpen()
{
    if (!list_open_) {
        list_->begin();
        list_open_ = true;
    }
}

void PlumeRsxBackend::EnsureListClosedAndSubmitted(RenderCommandSemaphore* wait_sem,
                                                   RenderCommandSemaphore* signal_sem)
{
    if (!list_open_) return;
    list_->end();
    list_open_ = false;

    const RenderCommandList* cmd_list = list_.get();
    /* Present()'s bug (fixed here): submitting with no signal semaphore
     * while swap_chain_->present() is later told to wait on one that
     * nothing ever signals is a permanent GPU-side wait -- confirmed via a
     * backtrace stuck in VulkanSwapChain::present()'s syncobj wait. Whoever
     * calls this with real semaphores is responsible for keeping them alive
     * until the GPU work they guard has completed (Present() already does,
     * via waitForCommandFence below). */
    queue_->executeCommandLists(&cmd_list, 1,
                                wait_sem ? &wait_sem : nullptr, wait_sem ? 1 : 0,
                                signal_sem ? &signal_sem : nullptr, signal_sem ? 1 : 0,
                                fence_.get());
    queue_->waitForCommandFence(fence_.get());
}

void PlumeRsxBackend::SubmitAndWait(uint32_t /*reason*/)
{
    EnsureListClosedAndSubmitted();
}

uint32_t PlumeRsxBackend::ColorTargetCreate(rsx_be_format fmt, uint32_t w, uint32_t h,
                                            const void* /*seed*/, uint32_t /*seed_row_bytes*/)
{
    if (w == 0 || h == 0) return 0;

    Surface surface;
    surface.width = w;
    surface.height = h;
    surface.format = ToPlumeFormat(fmt);

    RenderTextureDesc desc = RenderTextureDesc::Texture2D(w, h, 1, surface.format,
                                                          RenderTextureFlag::RENDER_TARGET);
    surface.texture = device_->createTexture(desc);
    if (!surface.texture) {
        std::fprintf(stderr, "[plume backend] color_target_create: createTexture failed (%ux%u)\n", w, h);
        return 0;
    }

    const RenderTexture* color_attachment = surface.texture.get();
    RenderFramebufferDesc fb_desc;
    fb_desc.colorAttachments = &color_attachment;
    fb_desc.colorAttachmentsCount = 1;
    fb_desc.depthAttachment = nullptr;
    surface.framebuffer = device_->createFramebuffer(fb_desc);
    if (!surface.framebuffer) {
        std::fprintf(stderr, "[plume backend] color_target_create: createFramebuffer failed\n");
        return 0;
    }

    /* TODO(Phase 2): upload `seed` (the guest's own CPU-initialised bytes
     * for this surface) via a staging buffer + copyBufferToTexture, the way
     * rsx_draw_backend's contract expects when a title samples a render
     * target before anything has drawn into it. Left un-seeded for now: the
     * surface starts as whatever the GPU gives a fresh allocation (usually
     * zero), not the guest's own bytes. */

    const uint32_t id = next_surface_id_++;
    surfaces_.emplace(id, std::move(surface));
    return id;
}

void PlumeRsxBackend::ColorTargetRelease(uint32_t surface)
{
    surfaces_.erase(surface);
}

bool PlumeRsxBackend::EnsurePipelineLayout()
{
    if (pipeline_layout_) return true;

    /* Set 0: the two constant buffers every RSX vertex/fragment program
     * pair uses (rsx_shader_msl.h's register(b0)/register(b1)). */
    const RenderDescriptorRange set0_ranges[2] = {
        RenderDescriptorRange(RenderDescriptorRangeType::CONSTANT_BUFFER, kVsConstantsBinding, 1),
        RenderDescriptorRange(RenderDescriptorRangeType::CONSTANT_BUFFER, kPsConstantsBinding, 1),
    };
    /* Set 1: the 16 fragment texture/sampler pairs (register(t0..15)/
     * (s0..15)), samplers placed after the textures in the same set. */
    const RenderDescriptorRange set1_ranges[2] = {
        RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, RSX_BE_MAX_TEXTURES),
        RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, RSX_BE_MAX_TEXTURES, RSX_BE_MAX_TEXTURES),
    };
    /* Set 2: the (much rarer) vertex texture/sampler pairs. */
    const RenderDescriptorRange set2_ranges[2] = {
        RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, RSX_BE_MAX_VERTEX_TEXTURES),
        RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, RSX_BE_MAX_VERTEX_TEXTURES, RSX_BE_MAX_VERTEX_TEXTURES),
    };
    const RenderDescriptorSetDesc set_descs[3] = {
        RenderDescriptorSetDesc(set0_ranges, 2),
        RenderDescriptorSetDesc(set1_ranges, 2),
        RenderDescriptorSetDesc(set2_ranges, 2),
    };

    RenderPipelineLayoutDesc layout_desc(nullptr, 0, set_descs, 3,
                                         /*isLocal=*/false, /*allowInputLayout=*/true);
    pipeline_layout_ = device_->createPipelineLayout(layout_desc);
    if (!pipeline_layout_) {
        std::fprintf(stderr, "[plume backend] createPipelineLayout failed\n");
    }
    return pipeline_layout_ != nullptr;
}

bool PlumeRsxBackend::CompileHlslToSpirv(const char* hlsl, int stage, std::vector<uint32_t>& out_words)
{
    /* RSX vertex/fragment programs are small (a handful of instructions to
     * a few hundred); 256K words is far more headroom than any real
     * decompiler output needs. */
    out_words.resize(65536);
    uint32_t word_count = 0;
    char log[512] = {};
    if (rsx_hlsl_to_spirv(hlsl, stage, out_words.data(), (uint32_t)out_words.size(), &word_count,
                         log, (uint32_t)sizeof(log)) != 0) {
        std::fprintf(stderr, "[plume backend] rsx_hlsl_to_spirv (stage %d) failed: %s\n", stage, log);
        return false;
    }
    out_words.resize(word_count);
    return true;
}

uint32_t PlumeRsxBackend::PipelineCreate(const char* vs_hlsl, const char* ps_hlsl,
                                         const rsx_be_render_state* rs,
                                         const rsx_vertex_layout_plan* layout,
                                         uint32_t vertex_stride, rsx_be_format rt_fmt, uint32_t rt_count)
{
    if (!vs_hlsl || !ps_hlsl || !rs || !layout) return 0;
    if (!EnsurePipelineLayout()) return 0;

    std::vector<uint32_t> vs_spirv, ps_spirv;
    if (!CompileHlslToSpirv(vs_hlsl, RSX_SHADER_STAGE_VERTEX, vs_spirv)) return 0;
    if (!CompileHlslToSpirv(ps_hlsl, RSX_SHADER_STAGE_FRAGMENT, ps_spirv)) return 0;

    Pipeline entry;
    entry.vertex_shader = device_->createShader(vs_spirv.data(), vs_spirv.size() * sizeof(uint32_t),
                                                "main", RenderShaderFormat::SPIRV);
    entry.pixel_shader = device_->createShader(ps_spirv.data(), ps_spirv.size() * sizeof(uint32_t),
                                               "main", RenderShaderFormat::SPIRV);
    if (!entry.vertex_shader || !entry.pixel_shader) {
        std::fprintf(stderr, "[plume backend] createShader failed\n");
        return 0;
    }

    /* rsx_vertex_compact.c ("each selected register remains a float4")
     * guarantees the vertex data draw() will hand this pipeline is already
     * expanded to one float4 per active attribute, tightly packed in
     * `layout->attrs` order -- so the input layout is always vec4-per-
     * attribute at consecutive 16-byte offsets, never the attribute's
     * original RSX wire format. `location` follows the same declaration
     * order glslang's mapIO assigned when rsx_hlsl_to_spirv compiled the
     * decompiler's ATTRn-named inputs (see rsx_shader_msl.h's binding
     * contract, which the SPIR-V path shares). */
    std::vector<RenderInputElement> input_elements;
    input_elements.reserve(layout->count);
    for (uint32_t i = 0; i < layout->count; ++i) {
        input_elements.emplace_back("ATTR", layout->attrs[i], /*location=*/i,
                                    RenderFormat::R32G32B32A32_FLOAT,
                                    /*slotIndex=*/0, /*alignedByteOffset=*/i * 16u);
    }
    const RenderInputSlot input_slot(0, vertex_stride, RenderInputSlotClassification::PER_VERTEX_DATA);

    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = pipeline_layout_.get();
    desc.vertexShader = entry.vertex_shader.get();
    desc.pixelShader = entry.pixel_shader.get();
    desc.inputSlots = &input_slot;
    desc.inputSlotsCount = 1;
    desc.inputElements = input_elements.data();
    desc.inputElementsCount = (uint32_t)input_elements.size();

    /* TODO(Phase 3, draw()): the engine's draw() call carries the topology
     * for that specific draw, but pipeline_create()'s own parameters don't
     * -- there is no per-topology cache key to build multiple pipeline
     * variants from yet. Default to the overwhelmingly common case; revisit
     * once draw() actually issues anything and a title exercises points/
     * lines/strips against a pipeline built here. */
    desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;

    desc.cullMode = rs->cull_enable
        ? ((rs->cull_face == 0x0404u || rs->cull_face == 0x0408u) ? RenderCullMode::FRONT : RenderCullMode::BACK)
        : RenderCullMode::NONE;
    desc.frontFace = (rs->front_face == 0x0901u) ? RenderFrontFace::COUNTER_CLOCKWISE : RenderFrontFace::CLOCKWISE;

    /* TODO(Phase 2, depth_target_create): depth/stencil testing is left
     * disabled regardless of `rs` until there is a real depth RenderTexture
     * to attach a format from -- depth_target_create() is still a stub (see
     * TrampDepthTargetCreate), so a pipeline with depthEnabled=true here
     * would have no depthTargetFormat to build against. */
    desc.depthEnabled = false;
    desc.stencilEnabled = false;

    RenderBlendDesc blend;
    if (rs->blend_enable) {
        blend.blendEnabled = true;
        blend.srcBlend = GlBlendFactorToPlume(rs->sf_rgb, false);
        blend.dstBlend = GlBlendFactorToPlume(rs->df_rgb, false);
        blend.blendOp = GlBlendOpToPlume(rs->eq_rgb);
        blend.srcBlendAlpha = GlBlendFactorToPlume(rs->sf_a, true);
        blend.dstBlendAlpha = GlBlendFactorToPlume(rs->df_a, true);
        blend.blendOpAlpha = GlBlendOpToPlume(rs->eq_a);
    }
    /* TODO: rs->color_mask's guest bit layout (which of ARGB maps to which
     * bit) isn't confirmed against this backend yet -- leaving every
     * channel writable is the safe default until it is, rather than risk a
     * silently wrong channel mask. */

    const RenderFormat color_format = ToPlumeFormat(rt_fmt);
    const uint32_t clamped_rt_count = rt_count ? rt_count : 1;
    for (uint32_t i = 0; i < clamped_rt_count && i < RenderGraphicsPipelineDesc::MaxRenderTargets; ++i) {
        desc.renderTargetBlend[i] = blend;
        desc.renderTargetFormat[i] = color_format;
    }
    desc.renderTargetCount = clamped_rt_count;

    entry.pipeline = device_->createGraphicsPipeline(desc);
    if (!entry.pipeline) {
        std::fprintf(stderr, "[plume backend] createGraphicsPipeline failed\n");
        return 0;
    }

    const uint32_t id = next_pipeline_id_++;
    pipelines_.emplace(id, std::move(entry));
    return id;
}

void PlumeRsxBackend::PipelineRelease(uint32_t pipeline)
{
    pipelines_.erase(pipeline);
}

void PlumeRsxBackend::ClearColor(uint32_t surface, const float rgba[4])
{
    Surface* s = FindSurface(surface);
    if (!s) return;

    EnsureListOpen();
    if (!s->color_write) {
        list_->barriers(RenderBarrierStage::GRAPHICS,
                        RenderTextureBarrier(s->texture.get(), RenderTextureLayout::COLOR_WRITE));
        s->color_write = true;
    }
    list_->setFramebuffer(s->framebuffer.get());
    const RenderViewport viewport(0.0f, 0.0f, (float)s->width, (float)s->height);
    const RenderRect scissor(0, 0, s->width, s->height);
    list_->setViewports(viewport);
    list_->setScissors(scissor);
    list_->clearColor(0, RenderColor(rgba[0], rgba[1], rgba[2], rgba[3]));
}

void PlumeRsxBackend::Present(uint32_t surface)
{
    Surface* s = FindSurface(surface);
    if (!s) return;

    EnsureListOpen();

    uint32_t image_index = 0;
    if (!swap_chain_->acquireTexture(acquire_sem_.get(), &image_index)) {
        std::fprintf(stderr, "[plume backend] present: acquireTexture failed\n");
        return;
    }
    RenderTexture* swap_texture = swap_chain_->getTexture(image_index);

    /* Source: whatever layout it is in (color-write from a clear/draw, or
     * fresh/undefined if nothing touched it this frame) -> COPY_SOURCE. */
    list_->barriers(RenderBarrierStage::COPY,
                    RenderTextureBarrier(s->texture.get(), RenderTextureLayout::COPY_SOURCE));
    s->color_write = false;
    list_->barriers(RenderBarrierStage::COPY,
                    RenderTextureBarrier(swap_texture, RenderTextureLayout::COPY_DEST));

    /* Phase 1 assumes the surface and the swapchain image are the same size
     * (the swapchain is created at the same width/height the caller passed
     * to Init, and cellGcm display buffers are created at that resolution
     * too) so a whole-texture copy is exact; a mismatched resize would need
     * copyTextureRegion or a blit instead. */
    list_->copyTexture(swap_texture, s->texture.get());

    list_->barriers(RenderBarrierStage::NONE,
                    RenderTextureBarrier(swap_texture, RenderTextureLayout::PRESENT));

    while (release_sems_.size() < swap_chain_->getTextureCount()) {
        release_sems_.emplace_back(device_->createCommandSemaphore());
    }
    RenderCommandSemaphore* signal_sem = release_sems_[image_index].get();

    /* Wait on acquire_sem_ (the swapchain image isn't necessarily ready the
     * instant acquireTexture() returns an index) and signal signal_sem so
     * that the present() call below -- which waits on it -- actually has
     * something to wait for. Without both of these this deadlocks inside
     * the driver: see EnsureListClosedAndSubmitted()'s comment. */
    EnsureListClosedAndSubmitted(acquire_sem_.get(), signal_sem);

    swap_chain_->present(image_index, &signal_sem, 1);
}

void PlumeRsxBackend::Readback(uint32_t /*surface*/, uint32_t /*x*/, uint32_t /*y*/,
                               uint32_t /*w*/, uint32_t /*h*/, void* /*out*/, uint32_t /*out_pitch*/)
{
    /* TODO(Phase 2): map a staging buffer, copyTextureRegion into it, wait
     * the fence, memcpy out. Not needed until something calls
     * rsx_draw_engine_readback_center() against this backend. */
    std::fprintf(stderr, "[plume backend] readback: not yet implemented\n");
}

int PlumeRsxBackend::PumpEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) return 0;
    }
    return 1;
}

/* ---- rsx_draw_backend vtable: thin C trampolines over the class above --- */

int TrampInit(void* user, uint32_t width, uint32_t height)
{
    return static_cast<PlumeRsxBackend*>(user)->Init(width, height, "ps3recomp");
}

void TrampShutdown(void* user)
{
    static_cast<PlumeRsxBackend*>(user)->Shutdown();
}

void TrampSubmitAndWait(void* user, uint32_t reason)
{
    static_cast<PlumeRsxBackend*>(user)->SubmitAndWait(reason);
}

uint32_t TrampTextureCreate(void*, rsx_be_format, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)
{
    /* TODO(Phase 2): wire rsx_hlsl_to_spirv + guest texture upload. Returning
     * 0 tells the engine "could not build", which it caches -- draws that
     * need this texture go untextured rather than crashing. */
    return 0;
}

void TrampTextureUpload(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, const void*, uint32_t, uint32_t) {}
void TrampTextureRelease(void*, uint32_t) {}

uint32_t TrampColorTargetCreate(void* user, rsx_be_format fmt, uint32_t w, uint32_t h,
                                const void* seed, uint32_t seed_row_bytes)
{
    return static_cast<PlumeRsxBackend*>(user)->ColorTargetCreate(fmt, w, h, seed, seed_row_bytes);
}

void TrampColorTargetRelease(void* user, uint32_t surface)
{
    static_cast<PlumeRsxBackend*>(user)->ColorTargetRelease(surface);
}

uint32_t TrampSurfaceView(void*, uint32_t, uint32_t, uint32_t) { return 0; }

uint32_t TrampDepthTargetCreate(void*, uint32_t, uint32_t) { return 0; }
void TrampDepthTargetRelease(void*, uint32_t) {}
uint32_t TrampDepthSnapshot(void*, uint32_t, uint32_t, uint32_t) { return 0; }

uint32_t TrampPipelineCreate(void* user, const char* vs_hlsl, const char* ps_hlsl,
                             const rsx_be_render_state* rs, const rsx_vertex_layout_plan* layout,
                             uint32_t vertex_stride, rsx_be_format rt_fmt, uint32_t rt_count)
{
    return static_cast<PlumeRsxBackend*>(user)->PipelineCreate(vs_hlsl, ps_hlsl, rs, layout,
                                                               vertex_stride, rt_fmt, rt_count);
}

void TrampPipelineRelease(void* user, uint32_t pipeline)
{
    static_cast<PlumeRsxBackend*>(user)->PipelineRelease(pipeline);
}

void TrampBindTargets(void*, const uint32_t*, uint32_t, uint32_t) {}
void TrampBindPipeline(void*, uint32_t) {}
void TrampBindVsConstants(void*, const void*, uint32_t) {}
void TrampBindPsConstants(void*, const void*, uint32_t) {}
void TrampBindTextures(void*, const uint32_t*, const rsx_be_sampler_desc*, uint32_t) {}
void TrampBindVertexTextures(void*, const uint32_t*, const rsx_be_sampler_desc*, uint32_t) {}
void TrampSetViewport(void*, float, float, float, float) {}
void TrampSetScissor(void*, uint32_t, uint32_t, uint32_t, uint32_t) {}
void TrampSetStencilRef(void*, uint32_t) {}

void TrampDraw(void*, rsx_topology, const void*, uint32_t, uint32_t, const uint32_t*, uint32_t)
{
    /* Pipelines can now be built (see TrampPipelineCreate), but nothing
     * binds one or issues a draw yet -- draw() itself is still a no-op:
     * the frame still clears and presents correctly, it is just empty of
     * guest geometry until bind_pipeline/bind_* and this are wired up. */
}

void TrampClearColor(void* user, uint32_t surface, const float rgba[4])
{
    static_cast<PlumeRsxBackend*>(user)->ClearColor(surface, rgba);
}

void TrampClearDepthStencil(void*, uint32_t, uint32_t, float, uint8_t) {}

void TrampPresent(void* user, uint32_t surface)
{
    static_cast<PlumeRsxBackend*>(user)->Present(surface);
}

void TrampReadback(void* user, uint32_t surface, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   void* out, uint32_t out_pitch)
{
    static_cast<PlumeRsxBackend*>(user)->Readback(surface, x, y, w, h, out, out_pitch);
}

} // namespace

extern "C" int rsx_draw_backend_plume_create(rsx_draw_backend* out, const char* window_title)
{
    if (!out) return -1;

    auto* backend_obj = new PlumeRsxBackend();
    /* Window/device creation happens in TrampInit (the engine calls init()
     * itself with the width/height it was given), not here -- this only
     * allocates the C++ object and wires the vtable. */
    (void)window_title;

    *out = rsx_draw_backend{};
    out->user = backend_obj;
    out->init = TrampInit;
    out->shutdown = TrampShutdown;
    out->submit_and_wait = TrampSubmitAndWait;
    out->texture_create = TrampTextureCreate;
    out->texture_upload = TrampTextureUpload;
    out->texture_release = TrampTextureRelease;
    out->color_target_create = TrampColorTargetCreate;
    out->color_target_release = TrampColorTargetRelease;
    out->surface_view = TrampSurfaceView;
    out->depth_target_create = TrampDepthTargetCreate;
    out->depth_target_release = TrampDepthTargetRelease;
    out->depth_snapshot = TrampDepthSnapshot;
    out->pipeline_create = TrampPipelineCreate;
    out->pipeline_release = TrampPipelineRelease;
    out->bind_targets = TrampBindTargets;
    out->bind_pipeline = TrampBindPipeline;
    out->bind_vs_constants = TrampBindVsConstants;
    out->bind_ps_constants = TrampBindPsConstants;
    out->bind_textures = TrampBindTextures;
    out->bind_vertex_textures = TrampBindVertexTextures;
    out->set_viewport = TrampSetViewport;
    out->set_scissor = TrampSetScissor;
    out->set_stencil_ref = TrampSetStencilRef;
    out->draw = TrampDraw;
    out->clear_color = TrampClearColor;
    out->clear_depth_stencil = TrampClearDepthStencil;
    out->present = TrampPresent;
    out->readback = TrampReadback;
    return 0;
}

extern "C" void rsx_draw_backend_plume_destroy(rsx_draw_backend* backend)
{
    if (!backend || !backend->user) return;
    delete static_cast<PlumeRsxBackend*>(backend->user);
    backend->user = nullptr;
}

extern "C" int rsx_draw_backend_plume_pump_events(rsx_draw_backend* backend)
{
    if (!backend || !backend->user) return 0;
    return static_cast<PlumeRsxBackend*>(backend->user)->PumpEvents();
}
