#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "mvob_parser.h"

int main(void) {
    printf("=== Running MVOB Parser Tests ===\n");

    /* 1. Struct size check */
    printf("Checking mvob_video_ref_t size... ");
    assert(sizeof(mvob_video_ref_t) == 40);
    printf("OK (%zu bytes)\n", sizeof(mvob_video_ref_t));

    /* 2. Create dummy sector and parse */
    printf("Testing dummy video sector creation and parsing... ");
    uint8_t sector[DVD_SECTOR_SIZE];
    mvob_video_ref_t ref_in = {
        .magic = MVOB_MAGIC_BE,
        .version = 1,
        .vts_id = 1,
        .vobu_index = 42,
        .pts_nanos = 5000000000ULL, /* 5.0 seconds */
        .duration_nanos = 500000000U, /* 0.5 seconds */
        .is_idr = 1,
        .reserved = {0}
    };

    mvob_create_dummy_video_sector(sector, DVD_SECTOR_SIZE, &ref_in);
    assert(mvob_is_video_sector(sector, DVD_SECTOR_SIZE));

    mvob_video_ref_t ref_out;
    memset(&ref_out, 0, sizeof(ref_out));
    bool ok = mvob_parse_video_ref(sector, DVD_SECTOR_SIZE, &ref_out);
    assert(ok);
    assert(ref_out.magic == MVOB_MAGIC_BE);
    assert(ref_out.version == 1);
    assert(ref_out.vts_id == 1);
    assert(ref_out.vobu_index == 42);
    assert(ref_out.pts_nanos == 5000000000ULL);
    assert(ref_out.duration_nanos == 500000000U);
    assert(ref_out.is_idr == 1);
    printf("OK\n");

    /* 3. Test mvob_write_video_ref */
    printf("Testing mvob_write_video_ref in-place modification... ");
    ref_in.vobu_index = 99;
    ref_in.pts_nanos = 10000000000ULL;
    ref_in.is_idr = 0;
    ok = mvob_write_video_ref(sector, DVD_SECTOR_SIZE, &ref_in);
    assert(ok);

    memset(&ref_out, 0, sizeof(ref_out));
    ok = mvob_parse_video_ref(sector, DVD_SECTOR_SIZE, &ref_out);
    assert(ok);
    assert(ref_out.vobu_index == 99);
    assert(ref_out.pts_nanos == 10000000000ULL);
    assert(ref_out.is_idr == 0);
    printf("OK\n");

    /* 4. Non-video sector check */
    printf("Testing non-video sector rejection... ");
    uint8_t audio_sector[DVD_SECTOR_SIZE];
    memset(audio_sector, 0, DVD_SECTOR_SIZE);
    audio_sector[0] = 0x00; audio_sector[1] = 0x00; audio_sector[2] = 0x01; audio_sector[3] = 0xBA;
    audio_sector[13] = 0xF8;
    audio_sector[14] = 0x00; audio_sector[15] = 0x00; audio_sector[16] = 0x01; audio_sector[17] = 0xBD; /* Private 1 */
    assert(!mvob_is_video_sector(audio_sector, DVD_SECTOR_SIZE));
    assert(!mvob_parse_video_ref(audio_sector, DVD_SECTOR_SIZE, &ref_out));
    printf("OK\n");

    printf("All MVOB Parser tests passed!\n");
    return 0;
}
