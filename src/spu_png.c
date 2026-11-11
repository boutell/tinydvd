#include "spu_png.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <zlib.h>

static void write_be32(uint8_t *p, uint32_t val) {
    p[0] = (uint8_t)((val >> 24) & 0xFF);
    p[1] = (uint8_t)((val >> 16) & 0xFF);
    p[2] = (uint8_t)((val >> 8)  & 0xFF);
    p[3] = (uint8_t)(val & 0xFF);
}

uint8_t *spu_rgba_to_png(const uint32_t *rgba, int width, int height, size_t *out_size) {
    if (!rgba || width <= 0 || height <= 0 || !out_size) return NULL;
    *out_size = 0;

    size_t raw_row_bytes = (size_t)width * 4;
    size_t scanline_bytes = (raw_row_bytes + 1) * (size_t)height;
    uint8_t *scanlines = (uint8_t *)malloc(scanline_bytes);
    if (!scanlines) return NULL;

    /* Prepare raw scanlines with filter byte 0 (None) at the start of each row */
    for (int y = 0; y < height; y++) {
        uint8_t *row_dest = scanlines + (size_t)y * (raw_row_bytes + 1);
        row_dest[0] = 0; /* filter type None */
        const uint8_t *src_pixels = (const uint8_t *)(rgba + (size_t)y * width);
        memcpy(row_dest + 1, src_pixels, raw_row_bytes);
    }

    uLongf comp_bound = compressBound((uLong)scanline_bytes);
    uint8_t *comp_buf = (uint8_t *)malloc(comp_bound);
    if (!comp_buf) {
        free(scanlines);
        return NULL;
    }

    uLongf comp_len = comp_bound;
    /* Use fast compression (level 1) for real-time sub-millisecond encoding */
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    if (deflateInit(&strm, 1) != Z_OK) {
        free(scanlines);
        free(comp_buf);
        return NULL;
    }
    strm.next_in = scanlines;
    strm.avail_in = (uInt)scanline_bytes;
    strm.next_out = comp_buf;
    strm.avail_out = (uInt)comp_bound;
    deflate(&strm, Z_FINISH);
    comp_len = strm.total_out;
    deflateEnd(&strm);
    free(scanlines);

    /* Allocate final PNG buffer: 8 (sig) + 25 (IHDR) + (12 + comp_len) (IDAT) + 12 (IEND) */
    size_t png_len = 8 + 25 + (12 + comp_len) + 12;
    uint8_t *png = (uint8_t *)malloc(png_len);
    if (!png) {
        free(comp_buf);
        return NULL;
    }

    uint8_t *p = png;

    /* PNG Signature */
    static const uint8_t png_sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    memcpy(p, png_sig, 8);
    p += 8;

    /* IHDR Chunk: 13 bytes */
    write_be32(p, 13);
    p += 4;
    memcpy(p, "IHDR", 4);
    write_be32(p + 4, (uint32_t)width);
    write_be32(p + 8, (uint32_t)height);
    p[12] = 8; /* 8-bit depth */
    p[13] = 6; /* RGBA */
    p[14] = 0; /* deflate */
    p[15] = 0; /* no filter */
    p[16] = 0; /* no interlace */
    uint32_t ihdr_crc = (uint32_t)crc32(0L, p, 17);
    write_be32(p + 17, ihdr_crc);
    p += 21;

    /* IDAT Chunk */
    write_be32(p, (uint32_t)comp_len);
    p += 4;
    memcpy(p, "IDAT", 4);
    memcpy(p + 4, comp_buf, comp_len);
    uint32_t idat_crc = (uint32_t)crc32(0L, p, (uInt)(4 + comp_len));
    write_be32(p + 4 + comp_len, idat_crc);
    p += (8 + comp_len);
    free(comp_buf);

    /* IEND Chunk */
    write_be32(p, 0);
    p += 4;
    memcpy(p, "IEND", 4);
    uint32_t iend_crc = (uint32_t)crc32(0L, p, 4);
    write_be32(p + 4, iend_crc);
    p += 8;

    *out_size = (size_t)(p - png);
    return png;
}

char *spu_base64_encode(const uint8_t *data, size_t len) {
    if (!data || len == 0) return strdup("");

    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_len = 4 * ((len + 2) / 3);
    char *out = (char *)malloc(out_len + 1);
    if (!out) return NULL;

    size_t i = 0, j = 0;
    while (i < len) {
        size_t rem = len - i;
        uint32_t oct_a = data[i++];
        uint32_t oct_b = (rem > 1) ? data[i++] : 0;
        uint32_t oct_c = (rem > 2) ? data[i++] : 0;
        uint32_t triple = (oct_a << 16) | (oct_b << 8) | oct_c;

        out[j++] = tbl[(triple >> 18) & 0x3F];
        out[j++] = tbl[(triple >> 12) & 0x3F];
        out[j++] = (rem > 1) ? tbl[(triple >> 6) & 0x3F] : '=';
        out[j++] = (rem > 2) ? tbl[triple & 0x3F] : '=';
    }
    out[out_len] = '\0';
    return out;
}

char *spu_rgba_to_data_url(const uint32_t *rgba, int width, int height) {
    size_t png_size = 0;
    uint8_t *png = spu_rgba_to_png(rgba, width, height, &png_size);
    if (!png) return NULL;

    char *b64 = spu_base64_encode(png, png_size);
    free(png);
    if (!b64) return NULL;

    static const char prefix[] = "data:image/png;base64,";
    size_t prefix_len = sizeof(prefix) - 1;
    size_t b64_len = strlen(b64);
    char *data_url = (char *)malloc(prefix_len + b64_len + 1);
    if (data_url) {
        memcpy(data_url, prefix, prefix_len);
        memcpy(data_url + prefix_len, b64, b64_len + 1);
    }
    free(b64);
    return data_url;
}
