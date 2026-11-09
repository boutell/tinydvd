#include "spu_compositor.h"
#include "mvob_parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

static uint64_t spu_monotonic_nanos(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t byte_pos;
    int bit_pos; /* 7 down to 0 */
} bit_reader_t;

static inline void br_init(bit_reader_t *br, const uint8_t *data, size_t size, size_t start_byte) {
    br->data = data;
    br->size = size;
    br->byte_pos = start_byte;
    br->bit_pos = 7;
}

static inline uint32_t br_read_bits(bit_reader_t *br, int count) {
    uint32_t val = 0;
    for (int i = 0; i < count; i++) {
        if (br->byte_pos >= br->size) {
            val <<= (count - i);
            break;
        }
        uint8_t bit = (br->data[br->byte_pos] >> br->bit_pos) & 1;
        val = (val << 1) | bit;
        if (br->bit_pos == 0) {
            br->bit_pos = 7;
            br->byte_pos++;
        } else {
            br->bit_pos--;
        }
    }
    return val;
}

static inline void br_align_byte(bit_reader_t *br) {
    if (br->bit_pos != 7) {
        br->bit_pos = 7;
        br->byte_pos++;
    }
}

struct spu_compositor {
    uint32_t clut_rgba[16];
    uint8_t pixel_buf[SPU_WIDTH * SPU_HEIGHT]; /* 2-bit raw pixels: values 0..3 */
    uint32_t rgba_buf[SPU_WIDTH * SPU_HEIGHT]; /* 32-bit composite buffer */

    /* Active display commands */
    bool display_on;
    uint64_t expire_monotonic_ns;
    uint8_t color_idx[4];
    uint8_t contrast[4]; /* 0..15 */
    uint16_t start_col, end_col;
    uint16_t start_row, end_row;
    uint16_t top_field_offset;
    uint16_t btm_field_offset;

    /* SPU accumulation buffer */
    uint8_t *packet_accum;
    size_t accum_size;
    size_t accum_capacity;
    size_t expected_packet_len;
    uint8_t active_substream_id;

    /* Highlight */
    spu_highlight_t highlight;

    /* OpenGL texture */
    unsigned int gl_tex_id;
    bool tex_dirty;
    bool gl_initialized;
};

static inline uint32_t yuv_to_rgba(uint8_t y, uint8_t cr, uint8_t cb, uint8_t a) {
    int c = (int)y - 16;
    int d = (int)cb - 128;
    int e = (int)cr - 128;
    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;
    uint8_t r_u8 = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
    uint8_t g_u8 = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
    uint8_t b_u8 = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
    return (uint32_t)r_u8 | ((uint32_t)g_u8 << 8) | ((uint32_t)b_u8 << 16) | ((uint32_t)a << 24);
}

spu_compositor_t *spu_compositor_create(void) {
    spu_compositor_t *spu = (spu_compositor_t *)calloc(1, sizeof(spu_compositor_t));
    if (!spu) return NULL;

    /* Default 16-color CLUT (grayscale + standard DVD colors) */
    for (int i = 0; i < 16; i++) {
        uint8_t v = (uint8_t)(i * 17);
        spu->clut_rgba[i] = (uint32_t)v | ((uint32_t)v << 8) | ((uint32_t)v << 16) | 0xFF000000U;
    }

    spu->display_on = true;
    spu->end_col = SPU_WIDTH - 1;
    spu->end_row = SPU_HEIGHT - 1;
    spu->contrast[0] = 0;  /* Background is transparent */
    spu->contrast[1] = 15;
    spu->contrast[2] = 15;
    spu->contrast[3] = 15;
    spu->color_idx[0] = 0;
    spu->color_idx[1] = 1;
    spu->color_idx[2] = 2;
    spu->color_idx[3] = 3;

    spu->accum_capacity = 65536;
    spu->packet_accum = (uint8_t *)malloc(spu->accum_capacity);
    spu->active_substream_id = 0x20;
    spu->tex_dirty = true;

    return spu;
}

void spu_compositor_destroy(spu_compositor_t *spu) {
    if (!spu) return;
    if (spu->gl_initialized && spu->gl_tex_id) {
        glDeleteTextures(1, &spu->gl_tex_id);
    }
    if (spu->packet_accum) free(spu->packet_accum);
    free(spu);
}

void spu_compositor_set_active_stream(spu_compositor_t *spu, uint8_t substream_id) {
    if (!spu) return;
    if (spu->active_substream_id != substream_id) {
        spu->active_substream_id = substream_id;
        spu->accum_size = 0;
        spu->expected_packet_len = 0;
        spu->expire_monotonic_ns = 0;
        if (substream_id == 0x00) {
            memset(spu->pixel_buf, 0, sizeof(spu->pixel_buf));
            spu_compositor_update(spu);
        }
    }
}

void spu_compositor_clear(spu_compositor_t *spu) {
    if (!spu) return;
    memset(spu->pixel_buf, 0, sizeof(spu->pixel_buf));
    spu->accum_size = 0;
    spu->expected_packet_len = 0;
    spu->expire_monotonic_ns = 0;
    spu->highlight.active = false;
    spu_compositor_update(spu);
}

void spu_compositor_set_clut(spu_compositor_t *spu, const uint32_t *clut16) {
    if (!spu || !clut16) return;
    for (int i = 0; i < 16; i++) {
        uint32_t val = clut16[i];
        /* Standard DVD CLUT: 0x00, Y, Cr, Cb */
        uint8_t y  = (val >> 16) & 0xFF;
        uint8_t cr = (val >> 8) & 0xFF;
        uint8_t cb = val & 0xFF;
        spu->clut_rgba[i] = yuv_to_rgba(y, cr, cb, 0xFF);
    }
    spu_compositor_update(spu);
}

static void decode_rle_field(spu_compositor_t *spu, const uint8_t *data, size_t data_len,
                             size_t field_offset, int start_line, int line_step, int end_line,
                             int col_start, int col_end) {
    if (field_offset >= data_len) return;

    bit_reader_t br;
    br_init(&br, data, data_len, field_offset);

    int width = col_end - col_start + 1;
    if (width <= 0 || width > SPU_WIDTH) width = SPU_WIDTH;

    for (int line = start_line; line <= end_line; line += line_step) {
        if (line >= SPU_HEIGHT) break;
        int x = 0;
        while (x < width && br.byte_pos < data_len) {
            uint32_t code = br_read_bits(&br, 4);
            int count = 0;
            uint8_t color = 0;

            if (code >= 4) {
                /* 4 bits: count 1..3, color 0..3 */
                count = (code >> 2) & 0x3;
                color = code & 0x3;
            } else if (code >= 1) {
                /* 8 bits: count 4..15 */
                uint32_t code2 = br_read_bits(&br, 4);
                count = (code << 2) | ((code2 >> 2) & 0x3);
                color = code2 & 0x3;
            } else {
                /* Next bit pattern */
                uint32_t code2 = br_read_bits(&br, 4);
                if (code2 >= 4) {
                    /* 12 bits: count 16..63 */
                    uint32_t code3 = br_read_bits(&br, 4);
                    count = (code2 << 2) | ((code3 >> 2) & 0x3);
                    color = code3 & 0x3;
                } else if (code2 >= 1) {
                    /* 16 bits: count 64..255 */
                    uint32_t code3 = br_read_bits(&br, 8);
                    count = (code2 << 6) | ((code3 >> 2) & 0x3F);
                    color = code3 & 0x3;
                } else {
                    /* 16 bits: 0000 0000 nnnn nncc (if count == 0, rest of the line) */
                    uint32_t code3 = br_read_bits(&br, 8);
                    count = (code3 >> 2) & 0x3F;
                    if (count == 0) {
                        count = width - x;
                    }
                    color = code3 & 0x3;
                }
            }

            if (count <= 0) count = 1;
            for (int k = 0; k < count && (x + k) < width; k++) {
                int px = col_start + x + k;
                if (px >= 0 && px < SPU_WIDTH) {
                    spu->pixel_buf[line * SPU_WIDTH + px] = color;
                }
            }
            x += count;
        }
        br_align_byte(&br);
    }
}

static void parse_dcsq(spu_compositor_t *spu, const uint8_t *data, size_t total_size, size_t dcsq_offset) {
    size_t curr = dcsq_offset;
    while (curr + 4 <= total_size) {
        uint16_t dcsq_stm = ((uint16_t)data[curr] << 8) | data[curr + 1];
        uint16_t next_dcsq = ((uint16_t)data[curr + 2] << 8) | data[curr + 3];
        bool is_delayed = (curr > dcsq_offset && dcsq_stm > 0);
        size_t cmd_ptr = curr + 4;

        while (cmd_ptr < total_size) {
            uint8_t cmd = data[cmd_ptr++];
            if (cmd == 0xFF) { /* CMD_END */
                break;
            }
            switch (cmd) {
                case 0x00: /* FSTA_DSP */
                case 0x01: /* STA_DSP */
                    if (!is_delayed) {
                        spu->display_on = true;
                    }
                    break;
                case 0x02: /* STP_DSP */
                    if (is_delayed) {
                        /* Delayed STP_DSP: schedule subtitle expiration (1 tick = 1024/90000 s = 11.377ms) */
                        spu->expire_monotonic_ns = spu_monotonic_nanos() + (uint64_t)dcsq_stm * 11377778ULL;
                    } else {
                        spu->display_on = false;
                    }
                    break;
                case 0x03: /* SET_COLOR */
                    if (cmd_ptr + 2 <= total_size) {
                        if (!is_delayed) {
                            spu->color_idx[3] = (data[cmd_ptr] >> 4) & 0xF;
                            spu->color_idx[2] = data[cmd_ptr] & 0xF;
                            spu->color_idx[1] = (data[cmd_ptr + 1] >> 4) & 0xF;
                            spu->color_idx[0] = data[cmd_ptr + 1] & 0xF;
                        }
                        cmd_ptr += 2;
                    }
                    break;
                case 0x04: /* SET_CONTR */
                    if (cmd_ptr + 2 <= total_size) {
                        if (!is_delayed) {
                            spu->contrast[3] = (data[cmd_ptr] >> 4) & 0xF;
                            spu->contrast[2] = data[cmd_ptr] & 0xF;
                            spu->contrast[1] = (data[cmd_ptr + 1] >> 4) & 0xF;
                            spu->contrast[0] = data[cmd_ptr + 1] & 0xF;
                        }
                        cmd_ptr += 2;
                    }
                    break;
                case 0x05: /* SET_DAREA */
                    if (cmd_ptr + 6 <= total_size) {
                        if (!is_delayed) {
                            spu->start_col = ((data[cmd_ptr] << 4) | (data[cmd_ptr + 1] >> 4)) & 0xFFF;
                            spu->end_col   = (((data[cmd_ptr + 1] & 0x0F) << 8) | data[cmd_ptr + 2]) & 0xFFF;
                            spu->start_row = ((data[cmd_ptr + 3] << 4) | (data[cmd_ptr + 4] >> 4)) & 0xFFF;
                            spu->end_row   = (((data[cmd_ptr + 4] & 0x0F) << 8) | data[cmd_ptr + 5]) & 0xFFF;
                        }
                        cmd_ptr += 6;
                    }
                    break;
                case 0x06: /* SET_DSPXA */
                    if (cmd_ptr + 4 <= total_size) {
                        if (!is_delayed) {
                            spu->top_field_offset = ((uint16_t)data[cmd_ptr] << 8) | data[cmd_ptr + 1];
                            spu->btm_field_offset = ((uint16_t)data[cmd_ptr + 2] << 8) | data[cmd_ptr + 3];
                        }
                        cmd_ptr += 4;
                    }
                    break;
                case 0x07: /* CHG_COLCON */
                    /* Skip variable-length colcon table */
                    if (cmd_ptr + 2 <= total_size) {
                        uint16_t bytes = ((uint16_t)data[cmd_ptr] << 8) | data[cmd_ptr + 1];
                        cmd_ptr += 2 + bytes;
                    }
                    break;
                default:
                    /* Unknown command, end block */
                    cmd_ptr = total_size;
                    break;
            }
        }

        if (next_dcsq == curr || next_dcsq >= total_size) {
            break;
        }
        curr = next_dcsq;
    }
}

void spu_compositor_feed_spu_data(spu_compositor_t *spu, const uint8_t *data, size_t len, uint32_t pts) {
    (void)pts;
    if (!spu || !data || len < 4) return;

    uint16_t total_size = ((uint16_t)data[0] << 8) | data[1];
    uint16_t data_offset = ((uint16_t)data[2] << 8) | data[3];

    if (total_size > len || data_offset >= total_size) return;

    memset(spu->pixel_buf, 0, sizeof(spu->pixel_buf));
    spu->expire_monotonic_ns = 0;
    spu->top_field_offset = 4;
    spu->btm_field_offset = (data_offset > 4) ? (4 + (data_offset - 4) / 2) : 4;

    parse_dcsq(spu, data, total_size, data_offset);

    /* Decode top field (even scanlines) */
    decode_rle_field(spu, data, total_size, spu->top_field_offset,
                     spu->start_row, 2, spu->end_row, spu->start_col, spu->end_col);

    /* Decode bottom field (odd scanlines) */
    decode_rle_field(spu, data, total_size, spu->btm_field_offset,
                     spu->start_row + 1, 2, spu->end_row, spu->start_col, spu->end_col);

    spu_compositor_update(spu);
}

void spu_compositor_feed_sector(spu_compositor_t *spu, const uint8_t *sector, size_t sector_len) {
    if (!spu || !sector || sector_len < DVD_SECTOR_SIZE) return;

    /* Check pack header: 0x00 0x00 0x01 0xBA */
    if (sector[0] != 0x00 || sector[1] != 0x00 || sector[2] != 0x01 || sector[3] != 0xBA) return;
    size_t pack_stuffing = sector[13] & 0x07;
    size_t pes_offset = 14 + pack_stuffing;
    if (pes_offset + 9 > sector_len) return;

    /* Check PES start code prefix: 0x00 0x00 0x01 */
    if (sector[pes_offset] != 0x00 || sector[pes_offset + 1] != 0x00 || sector[pes_offset + 2] != 0x01) return;

    uint8_t stream_id = sector[pes_offset + 3];
    if (stream_id != 0xBD) return; /* Private stream 1 */

    uint16_t pes_packet_len = ((uint16_t)sector[pes_offset + 4] << 8) | sector[pes_offset + 5];
    size_t pes_end = pes_offset + 6 + pes_packet_len;
    if (pes_end > sector_len || pes_packet_len == 0) {
        pes_end = sector_len;
    }

    uint8_t pes_header_data_len = sector[pes_offset + 8];
    size_t payload_offset = pes_offset + 9 + pes_header_data_len;
    if (payload_offset + 1 >= pes_end) return;

    uint8_t substream_id = sector[payload_offset];
    if (substream_id < 0x20 || substream_id > 0x3F) return; /* SPU streams are 0x20..0x3F */
    if (spu->active_substream_id == 0x00 || substream_id != spu->active_substream_id) return;

    const uint8_t *spu_payload = sector + payload_offset + 1;
    size_t spu_payload_len = pes_end - (payload_offset + 1);

    /* If PES header carries a PTS flag, this sector begins a new SPU packet */
    if ((sector[pes_offset + 7] & 0x80) != 0) {
        spu->accum_size = 0;
        spu->expected_packet_len = 0;
    }

    /* If this packet starts an SPU unit, first 2 bytes are total SPU size */
    if (spu->accum_size == 0 && spu_payload_len >= 4) {
        spu->expected_packet_len = ((size_t)spu_payload[0] << 8) | spu_payload[1];
    }

    if (spu->accum_size + spu_payload_len > spu->accum_capacity) {
        spu->accum_capacity = (spu->accum_size + spu_payload_len) * 2;
        spu->packet_accum = (uint8_t *)realloc(spu->packet_accum, spu->accum_capacity);
    }

    memcpy(spu->packet_accum + spu->accum_size, spu_payload, spu_payload_len);
    spu->accum_size += spu_payload_len;

    if (spu->expected_packet_len > 0 && spu->accum_size >= spu->expected_packet_len) {
        spu_compositor_feed_spu_data(spu, spu->packet_accum, spu->expected_packet_len, 0);
        spu->accum_size = 0;
        spu->expected_packet_len = 0;
    }
}

void spu_compositor_set_highlight(spu_compositor_t *spu, uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint32_t palette) {
    if (!spu) return;
    spu->highlight.active = true;
    spu->highlight.sx = sx;
    spu->highlight.sy = sy;
    spu->highlight.ex = ex;
    spu->highlight.ey = ey;
    spu->highlight.palette = palette;
    spu_compositor_update(spu);
}

void spu_compositor_clear_highlight(spu_compositor_t *spu) {
    if (!spu) return;
    spu->highlight.active = false;
    spu_compositor_update(spu);
}

void spu_compositor_update(spu_compositor_t *spu) {
    if (!spu) return;

    memset(spu->rgba_buf, 0, sizeof(spu->rgba_buf));

    uint8_t hl_color[4] = {0};
    uint8_t hl_contrast[4] = {0};
    bool hl_on = spu->highlight.active;

    if (hl_on) {
        uint16_t color_val = (spu->highlight.palette >> 16) & 0xFFFF;
        uint16_t contrast_val = spu->highlight.palette & 0xFFFF;
        for (int p = 0; p < 4; p++) {
            hl_color[p] = (color_val >> (p * 4)) & 0x0F;
            hl_contrast[p] = (contrast_val >> (p * 4)) & 0x0F;
        }
    }

    int highlighted_spu_pixels = 0;
    for (int y = 0; y < SPU_HEIGHT; y++) {
        for (int x = 0; x < SPU_WIDTH; x++) {
            uint8_t px = spu->pixel_buf[y * SPU_WIDTH + x];
            bool in_hl = hl_on && (x >= spu->highlight.sx && x <= spu->highlight.ex &&
                                   y >= spu->highlight.sy && y <= spu->highlight.ey);

            uint8_t c_idx = in_hl ? hl_color[px] : spu->color_idx[px];
            uint8_t contrast = in_hl ? hl_contrast[px] : (spu->display_on ? spu->contrast[px] : 0);

            if (contrast > 0) {
                uint32_t base_rgba = spu->clut_rgba[c_idx & 0x0F];
                uint8_t alpha = (uint8_t)(contrast * 17); /* Scale 0..15 to 0..255 */
                uint32_t pixel = (base_rgba & 0x00FFFFFFU) | ((uint32_t)alpha << 24);
                spu->rgba_buf[y * SPU_WIDTH + x] = pixel;
                if (in_hl) {
                    highlighted_spu_pixels++;
                }
            }
        }
    }

    /* Fallback: if no SPU bitmap pixels exist inside the active button rect
     * (e.g. unit test or DVD without SPU subpictures), draw a 2-pixel highlight border. */
    if (hl_on && highlighted_spu_pixels == 0) {
        int x1 = spu->highlight.sx;
        int y1 = spu->highlight.sy;
        int x2 = spu->highlight.ex;
        int y2 = spu->highlight.ey;
        if (x1 < SPU_WIDTH && y1 < SPU_HEIGHT && x2 > x1 && y2 > y1) {
            if (x2 >= SPU_WIDTH) x2 = SPU_WIDTH - 1;
            if (y2 >= SPU_HEIGHT) y2 = SPU_HEIGHT - 1;
            uint32_t border_color = 0xCC00D4FFU; /* Warm gold/amber semi-transparent border (ABGR) */
            for (int t = 0; t < 2; t++) {
                int yt1 = (y1 + t < SPU_HEIGHT) ? (y1 + t) : y1;
                int yt2 = (y2 - t >= 0) ? (y2 - t) : y2;
                for (int x = x1; x <= x2; x++) {
                    spu->rgba_buf[yt1 * SPU_WIDTH + x] = border_color;
                    spu->rgba_buf[yt2 * SPU_WIDTH + x] = border_color;
                }
                int xt1 = (x1 + t < SPU_WIDTH) ? (x1 + t) : x1;
                int xt2 = (x2 - t >= 0) ? (x2 - t) : x2;
                for (int y = y1; y <= y2; y++) {
                    spu->rgba_buf[y * SPU_WIDTH + xt1] = border_color;
                    spu->rgba_buf[y * SPU_WIDTH + xt2] = border_color;
                }
            }
        }
    }

    spu->tex_dirty = true;
}

const uint32_t *spu_compositor_get_rgba(spu_compositor_t *spu, int *out_w, int *out_h) {
    if (!spu) return NULL;
    if (out_w) *out_w = SPU_WIDTH;
    if (out_h) *out_h = SPU_HEIGHT;
    return spu->rgba_buf;
}

void spu_compositor_render_gl(spu_compositor_t *spu, int win_w, int win_h) {
    if (!spu) return;

    /* Expire timed movie subtitles (delayed STP_DSP) when their duration elapses */
    if (!spu->highlight.active && spu->display_on && spu->expire_monotonic_ns > 0) {
        if (spu_monotonic_nanos() >= spu->expire_monotonic_ns) {
            spu->display_on = false;
            spu->expire_monotonic_ns = 0;
            memset(spu->pixel_buf, 0, sizeof(spu->pixel_buf));
            spu_compositor_update(spu);
        }
    }

    /* Reset OpenGL state after libmpv's programmable shader pass so our
     * fixed-function 2D overlay quad renders cleanly on texture unit 0. */
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    if (!spu->gl_initialized) {
        glGenTextures(1, &spu->gl_tex_id);
        glBindTexture(GL_TEXTURE_2D, spu->gl_tex_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SPU_WIDTH, SPU_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, spu->rgba_buf);
        spu->gl_initialized = true;
        spu->tex_dirty = false;
    } else if (spu->tex_dirty) {
        glBindTexture(GL_TEXTURE_2D, spu->gl_tex_id);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SPU_WIDTH, SPU_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, spu->rgba_buf);
        spu->tex_dirty = false;
    }

    glViewport(0, 0, win_w, win_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, spu->gl_tex_id);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, win_w, win_h, 0, -1, 1);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    /* Match libmpv's 16:9 keepaspect viewport inside (win_w, win_h) */
    float target_aspect = 16.0f / 9.0f;
    float win_aspect = (win_h > 0) ? ((float)win_w / (float)win_h) : target_aspect;
    float vp_x = 0.0f, vp_y = 0.0f, vp_w = (float)win_w, vp_h = (float)win_h;
    if (win_aspect > target_aspect) {
        vp_w = (float)win_h * target_aspect;
        vp_x = ((float)win_w - vp_w) * 0.5f;
    } else if (win_aspect < target_aspect) {
        vp_h = (float)win_w / target_aspect;
        vp_y = ((float)win_h - vp_h) * 0.5f;
    }

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(vp_x,        vp_y);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(vp_x + vp_w, vp_y);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(vp_x + vp_w, vp_y + vp_h);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(vp_x,        vp_y + vp_h);
    glEnd();

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);

    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
}
