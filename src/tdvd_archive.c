#include "tdvd_archive.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

typedef struct {
    char     name[256];
    uint64_t data_offset;
    uint64_t size;
} tdvd_entry_t;

struct tdvd_archive {
    char         *archive_path;
    char          ifo_dir[512];
    tdvd_entry_t *entries;
    size_t        entry_count;
    size_t        entry_capacity;
};

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p) {
    return (uint64_t)read_le32(p) | ((uint64_t)read_le32(p + 4) << 32);
}

bool tdvd_is_archive_path(const char *path) {
    if (!path) return false;
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return false;
    }
    const char *dot = strrchr(path, '.');
    if (dot && (strcasecmp(dot, ".tdvd") == 0 || strcasecmp(dot, ".tinydvd") == 0)) {
        return true;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t magic[4] = {0};
    size_t n = fread(magic, 1, 4, f);
    fclose(f);
    return (n == 4 && magic[0] == 'P' && magic[1] == 'K' && magic[2] == 0x03 && magic[3] == 0x04);
}

static const char *basename_ptr(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? (slash + 1) : path;
}

static bool has_ifo_or_bup_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return (strcasecmp(dot, ".IFO") == 0 || strcasecmp(dot, ".BUP") == 0);
}

static void remove_ifo_dir(const char *dir_path) {
    if (!dir_path || !dir_path[0]) return;
    char cmd[1024];
    /* Remove only .IFO/.BUP files and the temp directory itself */
    for (int vts = 0; vts < 100; vts++) {
        char p[600];
        if (vts == 0) {
            snprintf(p, sizeof(p), "%s/VIDEO_TS.IFO", dir_path); unlink(p);
            snprintf(p, sizeof(p), "%s/VIDEO_TS.BUP", dir_path); unlink(p);
        } else {
            snprintf(p, sizeof(p), "%s/VTS_%02d_0.IFO", dir_path, vts); unlink(p);
            snprintf(p, sizeof(p), "%s/VTS_%02d_0.BUP", dir_path, vts); unlink(p);
        }
    }
    (void)cmd;
    rmdir(dir_path);
}

tdvd_archive_t *tdvd_archive_open(const char *archive_path) {
    if (!archive_path) return NULL;
    FILE *f = fopen(archive_path, "rb");
    if (!f) {
        fprintf(stderr, "[tdvd_archive] Cannot open '%s'\n", archive_path);
        return NULL;
    }

    if (fseeko(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    off_t file_size = ftello(f);
    if (file_size < 22) {
        fclose(f);
        return NULL;
    }

    /* Search last 65 KB for End of Central Directory (PK\x05\x06) */
    size_t tail_len = (file_size > 65557) ? 65557 : (size_t)file_size;
    uint8_t *tail = (uint8_t *)malloc(tail_len);
    if (!tail) {
        fclose(f);
        return NULL;
    }

    fseeko(f, file_size - (off_t)tail_len, SEEK_SET);
    if (fread(tail, 1, tail_len, f) != tail_len) {
        free(tail);
        fclose(f);
        return NULL;
    }

    ssize_t eocd_pos = -1;
    for (ssize_t i = (ssize_t)tail_len - 22; i >= 0; i--) {
        if (tail[i] == 'P' && tail[i + 1] == 'K' && tail[i + 2] == 0x05 && tail[i + 3] == 0x06) {
            eocd_pos = i;
            break;
        }
    }

    if (eocd_pos < 0) {
        fprintf(stderr, "[tdvd_archive] Invalid .tdvd archive (ZIP EOCD not found): %s\n", archive_path);
        free(tail);
        fclose(f);
        return NULL;
    }

    uint64_t cd_num_entries = read_le16(tail + eocd_pos + 10);
    uint64_t cd_size        = read_le32(tail + eocd_pos + 12);
    uint64_t cd_offset      = read_le32(tail + eocd_pos + 16);

    /* Check for ZIP64 End of Central Directory Locator (PK\x06\x07) 20 bytes before EOCD */
    if (eocd_pos >= 20 &&
        tail[eocd_pos - 20] == 'P' && tail[eocd_pos - 19] == 'K' &&
        tail[eocd_pos - 18] == 0x06 && tail[eocd_pos - 17] == 0x07) {
        uint64_t zip64_eocd_offset = read_le64(tail + eocd_pos - 20 + 8);
        uint8_t z64_buf[56];
        if (fseeko(f, (off_t)zip64_eocd_offset, SEEK_SET) == 0 &&
            fread(z64_buf, 1, sizeof(z64_buf), f) == sizeof(z64_buf) &&
            z64_buf[0] == 'P' && z64_buf[1] == 'K' && z64_buf[2] == 0x06 && z64_buf[3] == 0x06) {
            cd_num_entries = read_le64(z64_buf + 32);
            cd_size        = read_le64(z64_buf + 40);
            cd_offset      = read_le64(z64_buf + 48);
        }
    }
    free(tail);

    if (cd_offset + cd_size > (uint64_t)file_size) {
        fclose(f);
        return NULL;
    }

    uint8_t *cd_buf = (uint8_t *)malloc((size_t)cd_size);
    if (!cd_buf) {
        fclose(f);
        return NULL;
    }
    fseeko(f, (off_t)cd_offset, SEEK_SET);
    if (fread(cd_buf, 1, (size_t)cd_size, f) != (size_t)cd_size) {
        free(cd_buf);
        fclose(f);
        return NULL;
    }

    tdvd_archive_t *ar = (tdvd_archive_t *)calloc(1, sizeof(tdvd_archive_t));
    ar->archive_path = strdup(archive_path);
    snprintf(ar->ifo_dir, sizeof(ar->ifo_dir), "/tmp/tinydvd_ifo_XXXXXX");
    if (!mkdtemp(ar->ifo_dir)) {
        ar->ifo_dir[0] = '\0';
    }

    size_t pos = 0;
    for (uint64_t idx = 0; idx < cd_num_entries && pos + 46 <= (size_t)cd_size; idx++) {
        const uint8_t *ent = cd_buf + pos;
        if (ent[0] != 'P' || ent[1] != 'K' || ent[2] != 0x01 || ent[3] != 0x02) {
            break;
        }
        uint16_t method      = read_le16(ent + 10);
        uint64_t comp_size   = read_le32(ent + 20);
        uint64_t uncomp_size = read_le32(ent + 24);
        uint16_t fname_len   = read_le16(ent + 28);
        uint16_t extra_len   = read_le16(ent + 30);
        uint16_t comment_len = read_le16(ent + 32);
        uint64_t lh_offset   = read_le32(ent + 42);

        if (pos + 46 + fname_len + extra_len + comment_len > (size_t)cd_size) {
            break;
        }

        char raw_name[512] = {0};
        size_t copy_len = (fname_len < sizeof(raw_name) - 1) ? fname_len : (sizeof(raw_name) - 1);
        memcpy(raw_name, ent + 46, copy_len);
        raw_name[copy_len] = '\0';

        /* Parse ZIP64 extra field (header ID 0x0001) if any 32-bit field is 0xFFFFFFFF */
        const uint8_t *extra = ent + 46 + fname_len;
        size_t ep = 0;
        while (ep + 4 <= extra_len) {
            uint16_t eid  = read_le16(extra + ep);
            uint16_t elen = read_le16(extra + ep + 2);
            if (eid == 0x0001 && ep + 4 + elen <= extra_len) {
                size_t zp = ep + 4;
                if (uncomp_size == 0xFFFFFFFFULL && zp + 8 <= ep + 4 + elen) {
                    uncomp_size = read_le64(extra + zp);
                    zp += 8;
                }
                if (comp_size == 0xFFFFFFFFULL && zp + 8 <= ep + 4 + elen) {
                    comp_size = read_le64(extra + zp);
                    zp += 8;
                }
                if (lh_offset == 0xFFFFFFFFULL && zp + 8 <= ep + 4 + elen) {
                    lh_offset = read_le64(extra + zp);
                    zp += 8;
                }
            }
            ep += 4 + elen;
        }

        pos += 46 + fname_len + extra_len + comment_len;

        /* Skip directories or compressed entries (all .tdvd entries are ZIP_STORED method 0) */
        const char *bname = basename_ptr(raw_name);
        if (!bname[0] || raw_name[strlen(raw_name) - 1] == '/' || method != 0) {
            continue;
        }

        /* Read Local File Header (30 bytes) at lh_offset to determine exact data payload offset */
        uint8_t lh[30];
        if (fseeko(f, (off_t)lh_offset, SEEK_SET) != 0 || fread(lh, 1, 30, f) != 30) {
            continue;
        }
        if (lh[0] != 'P' || lh[1] != 'K' || lh[2] != 0x03 || lh[3] != 0x04) {
            continue;
        }
        uint16_t lh_fname_len = read_le16(lh + 26);
        uint16_t lh_extra_len = read_le16(lh + 28);
        uint64_t data_offset  = lh_offset + 30ULL + lh_fname_len + lh_extra_len;

        if (ar->entry_count >= ar->entry_capacity) {
            ar->entry_capacity = ar->entry_capacity ? (ar->entry_capacity * 2) : 64;
            ar->entries = (tdvd_entry_t *)realloc(ar->entries, ar->entry_capacity * sizeof(tdvd_entry_t));
        }
        tdvd_entry_t *dst = &ar->entries[ar->entry_count++];
        strncpy(dst->name, bname, sizeof(dst->name) - 1);
        dst->name[sizeof(dst->name) - 1] = '\0';
        dst->data_offset = data_offset;
        dst->size = uncomp_size;

        /* Stage small .IFO / .BUP files into ar->ifo_dir for libdvdread */
        if (ar->ifo_dir[0] && has_ifo_or_bup_ext(bname) && uncomp_size < 16 * 1024 * 1024) {
            char out_ifo_path[1024];
            snprintf(out_ifo_path, sizeof(out_ifo_path), "%s/%s", ar->ifo_dir, bname);
            FILE *out_f = fopen(out_ifo_path, "wb");
            if (out_f) {
                uint8_t *ifo_buf = (uint8_t *)malloc((size_t)uncomp_size);
                if (ifo_buf) {
                    fseeko(f, (off_t)data_offset, SEEK_SET);
                    if (fread(ifo_buf, 1, (size_t)uncomp_size, f) == (size_t)uncomp_size) {
                        fwrite(ifo_buf, 1, (size_t)uncomp_size, out_f);
                    }
                    free(ifo_buf);
                }
                fclose(out_f);
            }
        }
    }

    free(cd_buf);
    fclose(f);
    return ar;
}

void tdvd_archive_close(tdvd_archive_t *ar) {
    if (!ar) return;
    if (ar->ifo_dir[0]) {
        remove_ifo_dir(ar->ifo_dir);
    }
    if (ar->entries) free(ar->entries);
    if (ar->archive_path) free(ar->archive_path);
    free(ar);
}

bool tdvd_archive_find(const tdvd_archive_t *ar, const char *filename, uint64_t *out_offset, uint64_t *out_size) {
    if (!ar || !filename) return false;
    const char *target = basename_ptr(filename);
    for (size_t i = 0; i < ar->entry_count; i++) {
        if (strcasecmp(ar->entries[i].name, target) == 0) {
            if (out_offset) *out_offset = ar->entries[i].data_offset;
            if (out_size)   *out_size   = ar->entries[i].size;
            return true;
        }
    }
    return false;
}

const char *tdvd_archive_ifo_dir(const tdvd_archive_t *ar) {
    return (ar && ar->ifo_dir[0]) ? ar->ifo_dir : NULL;
}

const char *tdvd_archive_path(const tdvd_archive_t *ar) {
    return ar ? ar->archive_path : NULL;
}
