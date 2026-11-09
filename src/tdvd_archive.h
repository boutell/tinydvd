#ifndef TDVD_ARCHIVE_H
#define TDVD_ARCHIVE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct tdvd_archive tdvd_archive_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Returns true if path has a .tdvd or .tinydvd extension (or starts with PK\x03\x04 ZIP magic).
 */
bool tdvd_is_archive_path(const char *path);

/**
 * Open a .tdvd / .tinydvd uncompressed ZIP archive, parse its Central Directory in O(1),
 * and stage its small .IFO/.BUP navigation files into a temporary directory for libdvdread.
 */
tdvd_archive_t *tdvd_archive_open(const char *archive_path);

/**
 * Close the archive and remove its temporary .IFO staging directory.
 */
void tdvd_archive_close(tdvd_archive_t *ar);

/**
 * Look up a constituent file (case-insensitive basename, e.g. "VTS_04_1.mvob" or "VTS_04_VIDEO.mkv").
 * Returns true and populates out_offset and out_size if found.
 */
bool tdvd_archive_find(const tdvd_archive_t *ar, const char *filename, uint64_t *out_offset, uint64_t *out_size);

/**
 * Return the path to the temporary directory containing extracted .IFO/.BUP files for libdvdread.
 */
const char *tdvd_archive_ifo_dir(const tdvd_archive_t *ar);

/**
 * Return the path to the underlying .tdvd archive file.
 */
const char *tdvd_archive_path(const tdvd_archive_t *ar);

#ifdef __cplusplus
}
#endif

#endif /* TDVD_ARCHIVE_H */
