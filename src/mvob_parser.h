#ifndef MVOB_PARSER_H
#define MVOB_PARSER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define DVD_SECTOR_SIZE 2048

#define MVOB_MAGIC_BE 0x4D564F42U  /* "MVOB" in big-endian / literal */
#define MVOB_MAGIC_LE 0x424F564DU  /* "MVOB" in little-endian order */
#define MVOB_VERSION  1

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;           /* 0x4D564F42 ("MVOB") */
    uint16_t version;         /* 0x0001 */
    uint16_t vts_id;          /* Video Title Set index (e.g., 1 for VTS_01) */
    uint32_t vobu_index;      /* VOBU index within cell */
    uint64_t pts_nanos;       /* Target presentation timestamp in MKV */
    uint32_t duration_nanos;  /* VOBU duration in nanoseconds */
    uint8_t  is_idr;          /* 1 if VOBU start boundary (IDR frame) */
    uint8_t  reserved[15];    /* Zero padding */
} mvob_video_ref_t;
#pragma pack(pop)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Check if the given 2048-byte sector is an MPEG video pack (stream_id == 0xE0).
 */
bool mvob_is_video_sector(const uint8_t *sector, size_t len);

/**
 * Check if the given sector contains an MVOB video reference descriptor.
 */
bool mvob_has_video_ref(const uint8_t *sector, size_t len);

/**
 * Parse mvob_video_ref_t from a 2048-byte video sector.
 * Returns true on success, false if no valid descriptor was found.
 */
bool mvob_parse_video_ref(const uint8_t *sector, size_t len, mvob_video_ref_t *out_ref);

/**
 * Write mvob_video_ref_t into a 2048-byte video sector, preserving the
 * pack header and PES header, overwriting the payload with the descriptor,
 * and zero-padding the rest of the sector to 2048 bytes.
 * Returns true on success.
 */
bool mvob_write_video_ref(uint8_t *sector, size_t sector_len, const mvob_video_ref_t *ref);

/**
 * Create a fresh 2048-byte video sector containing the mvob_video_ref_t descriptor.
 */
void mvob_create_dummy_video_sector(uint8_t *out_sector, size_t sector_len, const mvob_video_ref_t *ref);

#ifdef __cplusplus
}
#endif

#endif /* MVOB_PARSER_H */
