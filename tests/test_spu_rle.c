#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "spu_compositor.h"

int main(void) {
    printf("=== Running SPU Compositor Tests ===\n");

    spu_compositor_t *spu = spu_compositor_create();
    assert(spu != NULL);

    int w = 0, h = 0;
    const uint32_t *rgba = spu_compositor_get_rgba(spu, &w, &h);
    assert(rgba != NULL);
    assert(w == SPU_WIDTH);
    assert(h == SPU_HEIGHT);

    /* Test CLUT update with custom palette */
    uint32_t test_clut[16];
    for (int i = 0; i < 16; i++) {
        test_clut[i] = ((uint32_t)(i * 15) << 16) | (128 << 8) | 128; /* Grayscale in DVD YUV */
    }
    spu_compositor_set_clut(spu, test_clut);

    /* Test highlight rect and palette */
    printf("Testing button highlight activation... ");
    /* Highlight box from (100, 100) to (200, 150) */
    uint32_t hl_palette = 0x12340FFFU; /* col3=1,col2=2,col1=3,col0=4, full contrast on 3..1 */
    spu_compositor_set_highlight(spu, 100, 100, 200, 150, hl_palette);

    rgba = spu_compositor_get_rgba(spu, &w, &h);
    /* In the fallback highlight border, border pixels must be non-zero */
    uint32_t border_pixel = rgba[100 * SPU_WIDTH + 100];
    assert((border_pixel >> 24) > 0); /* Has alpha */
    printf("OK\n");

    /* Clear highlight */
    printf("Testing clear highlight... ");
    spu_compositor_clear_highlight(spu);
    rgba = spu_compositor_get_rgba(spu, &w, &h);
    assert(rgba[100 * SPU_WIDTH + 100] == 0);
    printf("OK\n");

    /* PAL: a 576-line frame, with a button below NTSC's last line */
    printf("Testing PAL frame height... ");
    spu_compositor_set_frame_height(spu, SPU_PAL_HEIGHT);
    assert(spu_compositor_get_frame_height(spu) == SPU_PAL_HEIGHT);
    spu_compositor_set_highlight(spu, 100, 500, 200, 560, hl_palette);
    rgba = spu_compositor_get_rgba(spu, &w, &h);
    assert(h == SPU_PAL_HEIGHT);
    assert((rgba[500 * SPU_WIDTH + 100] >> 24) > 0);
    spu_compositor_clear_highlight(spu);
    rgba = spu_compositor_get_rgba(spu, &w, &h);
    assert(rgba[500 * SPU_WIDTH + 100] == 0);
    /* Anything else is treated as NTSC */
    spu_compositor_set_frame_height(spu, 1234);
    assert(spu_compositor_get_frame_height(spu) == SPU_HEIGHT);
    printf("OK\n");

    spu_compositor_destroy(spu);
    printf("All SPU Compositor tests passed!\n");
    return 0;
}
