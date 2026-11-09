#include "mvob_parser.h"
#include <string.h>

static inline uint16_t swap16(uint16_t x) {
    return (uint16_t)((x << 8) | (x >> 8));
}

static inline uint32_t swap32(uint32_t x) {
    return ((x << 24) & 0xff000000U) |
           ((x << 8)  & 0x00ff0000U) |
           ((x >> 8)  & 0x0000ff00U) |
           ((x >> 24) & 0x000000ffU);
}

static inline uint64_t swap64(uint64_t x) {
    return (((uint64_t)swap32((uint32_t)(x & 0xffffffffU))) << 32) |
           (uint64_t)swap32((uint32_t)(x >> 32));
}

static size_t get_pes_payload_offset(const uint8_t *sector, size_t len) {
    if (len < DVD_SECTOR_SIZE) return 0;
    /* Check pack header: 0x00 0x00 0x01 0xBA */
    if (sector[0] != 0x00 || sector[1] != 0x00 || sector[2] != 0x01 || sector[3] != 0xBA) {
        return 0;
    }
    size_t pack_stuffing = sector[13] & 0x07;
    size_t pes_offset = 14 + pack_stuffing;
    if (pes_offset + 9 > len) return 0;

    /* Check PES start code prefix: 0x00 0x00 0x01 */
    if (sector[pes_offset] != 0x00 || sector[pes_offset + 1] != 0x00 || sector[pes_offset + 2] != 0x01) {
        return 0;
    }
    uint8_t stream_id = sector[pes_offset + 3];
    if (stream_id < 0xE0 || stream_id > 0xEF) {
        return 0;
    }

    /* PES header data length is at pes_offset + 8 */
    uint8_t pes_header_data_len = sector[pes_offset + 8];
    size_t payload_offset = pes_offset + 9 + pes_header_data_len;
    if (payload_offset + sizeof(mvob_video_ref_t) > len) {
        return 0;
    }
    return payload_offset;
}

bool mvob_is_video_sector(const uint8_t *sector, size_t len) {
    if (len < DVD_SECTOR_SIZE) return false;
    if (sector[0] != 0x00 || sector[1] != 0x00 || sector[2] != 0x01 || sector[3] != 0xBA) {
        return false;
    }
    size_t pack_stuffing = sector[13] & 0x07;
    size_t pes_offset = 14 + pack_stuffing;
    if (pes_offset + 4 > len) return false;

    if (sector[pes_offset] == 0x00 && sector[pes_offset + 1] == 0x00 && sector[pes_offset + 2] == 0x01) {
        uint8_t stream_id = sector[pes_offset + 3];
        return (stream_id >= 0xE0 && stream_id <= 0xEF);
    }
    return false;
}

bool mvob_has_video_ref(const uint8_t *sector, size_t len) {
    mvob_video_ref_t ref;
    return mvob_parse_video_ref(sector, len, &ref);
}

bool mvob_parse_video_ref(const uint8_t *sector, size_t len, mvob_video_ref_t *out_ref) {
    if (!sector || len < DVD_SECTOR_SIZE || !out_ref) {
        return false;
    }

    size_t payload_offset = get_pes_payload_offset(sector, len);
    const uint8_t *candidate = NULL;

    if (payload_offset > 0) {
        candidate = sector + payload_offset;
        uint32_t magic;
        memcpy(&magic, candidate, 4);
        if (magic == MVOB_MAGIC_BE || magic == MVOB_MAGIC_LE || memcmp(candidate, "MVOB", 4) == 0) {
            // Found at expected payload offset
        } else {
            candidate = NULL;
        }
    }

    /* Fallback search if not found directly at payload offset */
    if (!candidate) {
        for (size_t i = 14; i + sizeof(mvob_video_ref_t) <= len; i++) {
            if (memcmp(sector + i, "MVOB", 4) == 0 ||
                *(const uint32_t *)(sector + i) == MVOB_MAGIC_BE ||
                *(const uint32_t *)(sector + i) == MVOB_MAGIC_LE) {
                candidate = sector + i;
                break;
            }
        }
    }

    if (!candidate) {
        return false;
    }

    memcpy(out_ref, candidate, sizeof(mvob_video_ref_t));

    /* Check if byte-swapping is needed */
    if (out_ref->magic == MVOB_MAGIC_LE) {
        out_ref->version = swap16(out_ref->version);
        out_ref->vts_id = swap16(out_ref->vts_id);
        out_ref->vobu_index = swap32(out_ref->vobu_index);
        out_ref->pts_nanos = swap64(out_ref->pts_nanos);
        out_ref->duration_nanos = swap32(out_ref->duration_nanos);
        out_ref->magic = MVOB_MAGIC_BE;
    }

    return true;
}

bool mvob_write_video_ref(uint8_t *sector, size_t sector_len, const mvob_video_ref_t *ref) {
    if (!sector || sector_len < DVD_SECTOR_SIZE || !ref) {
        return false;
    }

    size_t payload_offset = get_pes_payload_offset(sector, sector_len);
    if (payload_offset == 0) {
        mvob_create_dummy_video_sector(sector, sector_len, ref);
        return true;
    }

    /* Copy descriptor to start of payload */
    memcpy(sector + payload_offset, ref, sizeof(mvob_video_ref_t));

    /* Zero pad remaining payload bytes to 2048 */
    size_t written_end = payload_offset + sizeof(mvob_video_ref_t);
    if (written_end < DVD_SECTOR_SIZE) {
        memset(sector + written_end, 0, DVD_SECTOR_SIZE - written_end);
    }

    return true;
}

void mvob_create_dummy_video_sector(uint8_t *out_sector, size_t sector_len, const mvob_video_ref_t *ref) {
    if (!out_sector || sector_len < DVD_SECTOR_SIZE) return;

    memset(out_sector, 0, DVD_SECTOR_SIZE);

    /* MPEG-2 Pack Header (14 bytes) */
    out_sector[0] = 0x00;
    out_sector[1] = 0x00;
    out_sector[2] = 0x01;
    out_sector[3] = 0xBA;
    out_sector[4] = 0x44; /* SCR bits */
    out_sector[5] = 0x00;
    out_sector[6] = 0x04;
    out_sector[7] = 0x00;
    out_sector[8] = 0x04;
    out_sector[9] = 0x01;
    out_sector[10] = 0x01;
    out_sector[11] = 0x89;
    out_sector[12] = 0xC3;
    out_sector[13] = 0xF8; /* stuffing length 0 */

    /* MPEG Video PES Header (9 bytes) */
    out_sector[14] = 0x00;
    out_sector[15] = 0x00;
    out_sector[16] = 0x01;
    out_sector[17] = 0xE0; /* Video stream 0 */
    /* PES packet length: 2048 - 14 (pack) - 6 (PES header prefix + len) = 2028 = 0x07EC */
    out_sector[18] = 0x07;
    out_sector[19] = 0xEC;
    out_sector[20] = 0x80; /* MPEG-2 flags */
    out_sector[21] = 0x00; /* No PTS/DTS */
    out_sector[22] = 0x00; /* PES header data length = 0 */

    /* Payload begins at offset 23 */
    if (ref) {
        memcpy(out_sector + 23, ref, sizeof(mvob_video_ref_t));
    }
}
