/*
 * ps3recomp - smoke test for the Plume/Vulkan rsx_draw_backend (Phase 1)
 *
 * Registers rsx_draw_backend_plume as the engine's backend, brings up a real
 * SDL2 + Vulkan window+device+swapchain, creates one color target sized to
 * match, clears it to a different color each frame, and presents it a few
 * times. This does not exercise pipelines/textures/draws (Phase 2, see
 * rsx_draw_backend_plume.cpp) -- it proves the part that Phase 1 claims:
 * a real frame reaches a real swapchain through Plume on Linux.
 *
 * Needs a Vulkan device (a software one, e.g. Mesa lavapipe, is enough) and
 * a display to create an SDL window against (a real one, or Xvfb).
 */
#include "plume_renderer/rsx_draw_backend_plume.h"

#include <stdio.h>

int main(void)
{
    rsx_draw_backend backend;
    if (rsx_draw_backend_plume_create(&backend, "ps3recomp smoke test") != 0) {
        fprintf(stderr, "rsx_draw_backend_plume_create failed\n");
        return 1;
    }

    const uint32_t width = 640, height = 480;
    if (backend.init(backend.user, width, height) != 0) {
        fprintf(stderr, "backend.init failed (no Vulkan device / no display?)\n");
        rsx_draw_backend_plume_destroy(&backend);
        return 1;
    }

    const uint32_t surface = backend.color_target_create(
        backend.user, RSX_BE_FMT_R8G8B8A8, width, height, NULL, 0);
    if (surface == 0) {
        fprintf(stderr, "color_target_create failed\n");
        backend.shutdown(backend.user);
        rsx_draw_backend_plume_destroy(&backend);
        return 1;
    }

    const float colors[3][4] = {
        {1.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 1.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 1.0f, 1.0f},
    };

    int frame;
    for (frame = 0; frame < 180; frame++) {
        if (!rsx_draw_backend_plume_pump_events(&backend)) {
            printf("quit requested at frame %d\n", frame);
            break;
        }
        backend.clear_color(backend.user, surface, colors[frame % 3]);
        backend.present(backend.user, surface);
        if (frame % 60 == 0) {
            printf("frame %d presented (color %d)\n", frame, frame % 3);
        }
    }

    printf("smoke test complete: %d frames presented through Plume/Vulkan\n", frame);

    backend.color_target_release(backend.user, surface);
    backend.shutdown(backend.user);
    rsx_draw_backend_plume_destroy(&backend);
    return 0;
}
