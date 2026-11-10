#ifndef SPU_COMPOSITOR_H
#define SPU_COMPOSITOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SPU_WIDTH      720
#define SPU_HEIGHT     480  /* NTSC frame height, the default */
#define SPU_PAL_HEIGHT 576  /* PAL frame height */
#define SPU_MAX_HEIGHT SPU_PAL_HEIGHT

typedef struct spu_highlight {
    bool active;
    uint16_t sx;
    uint16_t sy;
    uint16_t ex;
    uint16_t ey;
    uint32_t palette; /* High 16: colors (4x4bit), Low 16: contrast/alpha (4x4bit) */
} spu_highlight_t;

typedef struct spu_compositor spu_compositor_t;

#ifdef __cplusplus
extern "C" {
#endif

spu_compositor_t *spu_compositor_create(void);
void spu_compositor_destroy(spu_compositor_t *spu);

/**
 * Update the 16-color CLUT (Color Lookup Table) from IFO PGC palette.
 */
void spu_compositor_set_clut(spu_compositor_t *spu, const uint32_t *clut16);

/**
 * Set the active SPU substream ID (0x20..0x3F, or 0x00 to disable SPU).
 */
void spu_compositor_set_active_stream(spu_compositor_t *spu, uint8_t substream_id);

/**
 * Clear current SPU bitmap and highlight state (e.g. on domain/cell transitions).
 */
void spu_compositor_clear(spu_compositor_t *spu);

/**
 * Feed a DVD sector (2048 bytes). Extracts SPU subpicture packets (stream 0xBD, substream 0x20..0x3F).
 */
void spu_compositor_feed_sector(spu_compositor_t *spu, const uint8_t *sector, size_t sector_len);

/**
 * Feed complete or chunked SPU payload data.
 */
void spu_compositor_feed_spu_data(spu_compositor_t *spu, const uint8_t *data, size_t len, uint32_t pts);

/**
 * Set active button highlight rectangle and palette.
 */
void spu_compositor_set_highlight(spu_compositor_t *spu, uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint32_t palette);

/**
 * Clear any active button highlight.
 */
void spu_compositor_clear_highlight(spu_compositor_t *spu);

/**
 * Set the height of the video the overlay belongs to: SPU_HEIGHT (480, NTSC)
 * or SPU_PAL_HEIGHT (576, PAL). Menu and subtitle coordinates use it.
 */
void spu_compositor_set_frame_height(spu_compositor_t *spu, int height);
int spu_compositor_get_frame_height(spu_compositor_t *spu);

/**
 * Render the current SPU overlay onto the active OpenGL viewport, over the
 * video as mpv letterboxes it: display_aspect is the video's width / height.
 */
void spu_compositor_render_gl(spu_compositor_t *spu, int win_w, int win_h, float display_aspect);

/**
 * Get pointer to the composite RGBA pixel buffer: 720 wide, with out_h rows
 * (the frame height) in use. Rows are SPU_WIDTH pixels apart.
 */
const uint32_t *spu_compositor_get_rgba(spu_compositor_t *spu, int *out_w, int *out_h);

/**
 * Mark overlay as needing composite update.
 */
void spu_compositor_update(spu_compositor_t *spu);

#ifdef __cplusplus
}
#endif

#endif /* SPU_COMPOSITOR_H */
