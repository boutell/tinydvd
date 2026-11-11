#ifndef SPU_PNG_H
#define SPU_PNG_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Encode a 32-bit RGBA pixel buffer into a standard PNG byte buffer.
 * Caller must free() the returned buffer.
 * Returns NULL on failure.
 */
uint8_t *spu_rgba_to_png(const uint32_t *rgba, int width, int height, size_t *out_size);

/**
 * Base64 encode raw binary data.
 * Caller must free() the returned null-terminated string.
 */
char *spu_base64_encode(const uint8_t *data, size_t len);

/**
 * Encode RGBA buffer directly into a "data:image/png;base64,..." data URL.
 * Caller must free() the returned string.
 */
char *spu_rgba_to_data_url(const uint32_t *rgba, int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* SPU_PNG_H */
