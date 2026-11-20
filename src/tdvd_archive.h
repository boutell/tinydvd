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
 * Open a .tdvd / .tinydvd uncompressed ZIP archive and read its central directory (the index of members).
 */
tdvd_archive_t *tdvd_archive_open(const char *archive_path);

/**
 * Close the archive and release its index.
 */
void tdvd_archive_close(tdvd_archive_t *ar);

/**
 * Look up a constituent file (case-insensitive basename, e.g. "VTS_04_1.mvob" or "VTS_04_VIDEO.mkv").
 * Returns true and populates out_offset and out_size if found.
 */
bool tdvd_archive_find(const tdvd_archive_t *ar, const char *filename, uint64_t *out_offset, uint64_t *out_size);

/**
 * Read a small entry (up to 16 MB, e.g. an .IFO/.BUP file) from the archive into memory.
 * Caller owns the returned buffer and must call free(). Returns NULL if not found or too large.
 */
uint8_t *tdvd_archive_read_entry(const tdvd_archive_t *ar, const char *filename, size_t *out_size);

/**
 * Return the path to the underlying .tdvd archive file.
 */
const char *tdvd_archive_path(const tdvd_archive_t *ar);

#ifdef __cplusplus
}
#endif

#endif /* TDVD_ARCHIVE_H */
