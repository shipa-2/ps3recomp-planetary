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

    int PumpEvents();

    SDL_Window* window() const { return window_; }

private:
    void EnsureListOpen();
    void EnsureListClosedAndSubmitted();
    Surface* FindSurface(uint32_t id);

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

void PlumeRsxBackend::EnsureListClosedAndSubmitted()
{
    if (!list_open_) return;
    list_->end();
    list_open_ = false;

    const RenderCommandList* cmd_list = list_.get();
    queue_->executeCommandLists(&cmd_list, 1, nullptr, 0, nullptr, 0, fence_.get());
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

    EnsureListClosedAndSubmitted();

    while (release_sems_.size() < swap_chain_->getTextureCount()) {
        release_sems_.emplace_back(device_->createCommandSemaphore());
    }
    RenderCommandSemaphore* signal_sem = release_sems_[image_index].get();
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

uint32_t TrampPipelineCreate(void*, const char*, const char*, const rsx_be_render_state*,
                             const rsx_vertex_layout_plan*, uint32_t, rsx_be_format, uint32_t)
{
    /* TODO(Phase 2): rsx_hlsl_to_spirv() both stages, device_->createShader
     * with RenderShaderFormat::SPIRV, build a RenderGraphicsPipelineDesc from
     * `rs` + `layout` (see the triangle example for the shape) and call
     * createGraphicsPipeline. Everything the engine needs from Plume for
     * this already works (validated: HLSL -> SPIR-V -> Plume renders a real
     * frame through lavapipe/Xvfb in this same environment); what is left is
     * translating rsx_be_render_state/rsx_vertex_layout_plan's fields into
     * Plume's blend/depth/input-layout descriptors. */
    return 0;
}
void TrampPipelineRelease(void*, uint32_t) {}

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
    /* Phase 1 has no pipeline path (see TrampPipelineCreate), so every draw
     * is a no-op: the frame still clears and presents correctly, it is just
     * empty of guest geometry until Phase 2 lands. */
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
