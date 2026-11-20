#include "dvd_ifo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DVD_IFO_SECTOR_SIZE 2048U
#define DVD_IFO_MAX_BYTES   (16U * 1024U * 1024U)
#define DVD_PGC_HEADER_SIZE 236U
#define DVD_PCI_MIN_SIZE    980U

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static bool range_ok(size_t total, size_t offset, size_t len) {
    return offset <= total && len <= (total - offset);
}

static bool mul_u64_fits(uint64_t a, uint64_t b, size_t *out) {
    if (a != 0 && b > SIZE_MAX / a) {
        return false;
    }
    *out = (size_t)(a * b);
    return true;
}

static void free_pgc(dvd_pgc_t *pgc) {
    if (!pgc) return;
    if (pgc->command_tbl) {
        free(pgc->command_tbl->pre_cmds);
        free(pgc->command_tbl->post_cmds);
        free(pgc->command_tbl->cell_cmds);
        free(pgc->command_tbl);
    }
    free(pgc->program_map);
    free(pgc->cell_playback);
    free(pgc->cell_position);
    free(pgc);
}

static void free_pgcit(dvd_pgcit_t *pgcit) {
    if (!pgcit) return;
    if (pgcit->pgci_srp) {
        for (uint16_t i = 0; i < pgcit->nr_of_pgci_srp; i++) {
            free_pgc(pgcit->pgci_srp[i].pgc);
        }
        free(pgcit->pgci_srp);
    }
    free(pgcit);
}

static void free_pgci_ut(dvd_pgci_ut_t *ut) {
    if (!ut) return;
    if (ut->lu) {
        for (uint16_t i = 0; i < ut->nr_of_lus; i++) {
            free_pgcit(ut->lu[i].pgcit);
        }
        free(ut->lu);
    }
    free(ut);
}

static dvd_pgc_t *parse_pgc(const uint8_t *data, size_t size, size_t base) {
    if (!range_ok(size, base, DVD_PGC_HEADER_SIZE)) {
        return NULL;
    }

    dvd_pgc_t *pgc = (dvd_pgc_t *)calloc(1, sizeof(dvd_pgc_t));
    if (!pgc) return NULL;

    pgc->nr_of_programs = data[base + 0x02];
    pgc->nr_of_cells = data[base + 0x03];

    for (int i = 0; i < 32; i++) {
        pgc->subp_control[i] = be32(data + base + 0x1C + (size_t)i * 4U);
    }

    pgc->next_pgc_nr = be16(data + base + 0x9C);
    pgc->prev_pgc_nr = be16(data + base + 0x9E);
    pgc->goup_pgc_nr = be16(data + base + 0xA0);

    for (int i = 0; i < 16; i++) {
        pgc->palette[i] = be32(data + base + 0xA4 + (size_t)i * 4U);
    }

    uint16_t cmd_off = be16(data + base + 0xE4);
    uint16_t pmap_off = be16(data + base + 0xE6);
    uint16_t cpb_off = be16(data + base + 0xE8);
    uint16_t cpos_off = be16(data + base + 0xEA);

    /* Command table */
    if (cmd_off > 0 && range_ok(size, base + cmd_off, 8U)) {
        size_t cbase = base + cmd_off;
        uint16_t n_pre = be16(data + cbase + 0);
        uint16_t n_post = be16(data + cbase + 2);
        uint16_t n_cell = be16(data + cbase + 4);
        size_t pos = cbase + 8U;

        dvd_pgc_command_tbl_t *ct = (dvd_pgc_command_tbl_t *)calloc(1, sizeof(dvd_pgc_command_tbl_t));
        if (ct) {
            size_t avail_cmds = (size - pos) / 8U;
            if (n_pre > avail_cmds) n_pre = (uint16_t)avail_cmds;
            avail_cmds -= n_pre;
            if (n_post > avail_cmds) n_post = (uint16_t)avail_cmds;
            avail_cmds -= n_post;
            if (n_cell > avail_cmds) n_cell = (uint16_t)avail_cmds;

            if (n_pre > 0) {
                ct->pre_cmds = (dvd_vm_cmd_t *)calloc(n_pre, sizeof(dvd_vm_cmd_t));
                if (ct->pre_cmds) {
                    for (uint16_t i = 0; i < n_pre; i++) {
                        memcpy(ct->pre_cmds[i].bytes, data + pos, 8U);
                        pos += 8U;
                    }
                    ct->nr_of_pre = n_pre;
                }
            }
            if (n_post > 0) {
                ct->post_cmds = (dvd_vm_cmd_t *)calloc(n_post, sizeof(dvd_vm_cmd_t));
                if (ct->post_cmds) {
                    for (uint16_t i = 0; i < n_post; i++) {
                        memcpy(ct->post_cmds[i].bytes, data + pos, 8U);
                        pos += 8U;
                    }
                    ct->nr_of_post = n_post;
                }
            }
            if (n_cell > 0) {
                ct->cell_cmds = (dvd_vm_cmd_t *)calloc(n_cell, sizeof(dvd_vm_cmd_t));
                if (ct->cell_cmds) {
                    for (uint16_t i = 0; i < n_cell; i++) {
                        memcpy(ct->cell_cmds[i].bytes, data + pos, 8U);
                        pos += 8U;
                    }
                    ct->nr_of_cell = n_cell;
                }
            }
            pgc->command_tbl = ct;
        }
    }

    /* Program map */
    if (pgc->nr_of_programs > 0 && pmap_off > 0 && range_ok(size, base + pmap_off, 1U)) {
        size_t avail = size - (base + pmap_off);
        if (pgc->nr_of_programs > avail) {
            pgc->nr_of_programs = (uint8_t)avail;
        }
        if (pgc->nr_of_programs > 0) {
            pgc->program_map = (uint8_t *)malloc(pgc->nr_of_programs);
            if (pgc->program_map) {
                memcpy(pgc->program_map, data + base + pmap_off, pgc->nr_of_programs);
            } else {
                pgc->nr_of_programs = 0;
            }
        }
    } else if (pmap_off == 0) {
        pgc->nr_of_programs = 0;
    }

    /* Cell playback & cell position tables */
    if (pgc->nr_of_cells > 0 && cpb_off > 0 && range_ok(size, base + cpb_off, 24U)) {
        uint8_t n_cells = pgc->nr_of_cells;
        size_t max_cpb = (size - (base + cpb_off)) / 24U;
        if (n_cells > max_cpb) n_cells = (uint8_t)max_cpb;

        pgc->cell_playback = (dvd_cell_playback_t *)calloc(n_cells, sizeof(dvd_cell_playback_t));
        pgc->cell_position = (dvd_cell_position_t *)calloc(n_cells, sizeof(dvd_cell_position_t));
        if (!pgc->cell_playback || !pgc->cell_position) {
            free(pgc->cell_playback);
            free(pgc->cell_position);
            pgc->cell_playback = NULL;
            pgc->cell_position = NULL;
            pgc->nr_of_cells = 0;
        } else {
            for (uint8_t c = 0; c < n_cells; c++) {
                const uint8_t *cp = data + base + cpb_off + (size_t)c * 24U;
                pgc->cell_playback[c].still_time = cp[2];
                pgc->cell_playback[c].cell_cmd_nr = cp[3];
                pgc->cell_playback[c].first_sector = be32(cp + 8);
                pgc->cell_playback[c].last_sector = be32(cp + 20);
            }
            if (cpos_off > 0 && range_ok(size, base + cpos_off, 4U)) {
                size_t max_cpos = (size - (base + cpos_off)) / 4U;
                size_t cpos_count = (n_cells < max_cpos) ? n_cells : max_cpos;
                for (size_t c = 0; c < cpos_count; c++) {
                    const uint8_t *posp = data + base + cpos_off + c * 4U;
                    pgc->cell_position[c].vob_id_nr = be16(posp);
                    pgc->cell_position[c].cell_nr = posp[3];
                }
            }
            pgc->nr_of_cells = n_cells;
        }
    } else {
        pgc->nr_of_cells = 0;
    }

    return pgc;
}

static dvd_pgcit_t *parse_pgcit(const uint8_t *data, size_t size, size_t base) {
    if (!range_ok(size, base, 8U)) {
        return NULL;
    }
    uint16_t n_srp = be16(data + base);
    size_t max_srp = (size - (base + 8U)) / 8U;
    if (n_srp > max_srp) {
        n_srp = (uint16_t)max_srp;
    }

    dvd_pgcit_t *pgcit = (dvd_pgcit_t *)calloc(1, sizeof(dvd_pgcit_t));
    if (!pgcit) return NULL;

    pgcit->nr_of_pgci_srp = n_srp;
    if (n_srp == 0) return pgcit;

    pgcit->pgci_srp = (dvd_pgci_srp_t *)calloc(n_srp, sizeof(dvd_pgci_srp_t));
    if (!pgcit->pgci_srp) {
        free(pgcit);
        return NULL;
    }

    for (uint16_t i = 0; i < n_srp; i++) {
        const uint8_t *ent = data + base + 8U + (size_t)i * 8U;
        pgcit->pgci_srp[i].entry_id = ent[0];
        uint32_t pgc_rel = be32(ent + 4);
        if (pgc_rel <= size - base) {
            pgcit->pgci_srp[i].pgc = parse_pgc(data, size, base + (size_t)pgc_rel);
        }
    }
    return pgcit;
}

static dvd_pgci_ut_t *parse_pgci_ut(const uint8_t *data, size_t size, size_t base) {
    if (!range_ok(size, base, 8U)) {
        return NULL;
    }
    uint16_t n_lu = be16(data + base);
    size_t max_lu = (size - (base + 8U)) / 8U;
    if (n_lu > max_lu) {
        n_lu = (uint16_t)max_lu;
    }

    dvd_pgci_ut_t *ut = (dvd_pgci_ut_t *)calloc(1, sizeof(dvd_pgci_ut_t));
    if (!ut) return NULL;

    ut->nr_of_lus = n_lu;
    if (n_lu == 0) return ut;

    ut->lu = (dvd_pgci_lu_t *)calloc(n_lu, sizeof(dvd_pgci_lu_t));
    if (!ut->lu) {
        free(ut);
        return NULL;
    }

    for (uint16_t i = 0; i < n_lu; i++) {
        const uint8_t *ent = data + base + 8U + (size_t)i * 8U;
        uint32_t lu_rel = be32(ent + 4);
        if (lu_rel <= size - base) {
            ut->lu[i].pgcit = parse_pgcit(data, size, base + (size_t)lu_rel);
        }
    }
    return ut;
}

static dvd_tt_srpt_t *parse_tt_srpt(const uint8_t *data, size_t size, size_t base) {
    if (!range_ok(size, base, 8U)) {
        return NULL;
    }
    uint16_t n_tt = be16(data + base);
    size_t max_tt = (size - (base + 8U)) / 12U;
    if (n_tt > max_tt) {
        n_tt = (uint16_t)max_tt;
    }

    dvd_tt_srpt_t *srpt = (dvd_tt_srpt_t *)calloc(1, sizeof(dvd_tt_srpt_t));
    if (!srpt) return NULL;

    srpt->nr_of_srpts = n_tt;
    if (n_tt == 0) return srpt;

    srpt->title = (dvd_title_info_t *)calloc(n_tt, sizeof(dvd_title_info_t));
    if (!srpt->title) {
        free(srpt);
        return NULL;
    }

    for (uint16_t i = 0; i < n_tt; i++) {
        const uint8_t *ent = data + base + 8U + (size_t)i * 12U;
        srpt->title[i].nr_of_ptts = be16(ent + 2);
        srpt->title[i].title_set_nr = ent[6];
        srpt->title[i].vts_ttn = ent[7];
    }
    return srpt;
}

static dvd_vts_ptt_srpt_t *parse_vts_ptt_srpt(const uint8_t *data, size_t size, size_t base) {
    if (!range_ok(size, base, 8U)) {
        return NULL;
    }
    uint16_t n_srpts = be16(data + base);
    uint32_t last_byte = be32(data + base + 4);
    size_t max_srpts = (size - (base + 8U)) / 4U;
    if (n_srpts > max_srpts) {
        n_srpts = (uint16_t)max_srpts;
    }

    size_t table_len = (size_t)last_byte + 1U;
    if (table_len > size - base || last_byte == UINT32_MAX) {
        table_len = size - base;
    }

    dvd_vts_ptt_srpt_t *ptt_srpt = (dvd_vts_ptt_srpt_t *)calloc(1, sizeof(dvd_vts_ptt_srpt_t));
    if (!ptt_srpt) return NULL;

    ptt_srpt->nr_of_srpts = n_srpts;
    if (n_srpts == 0) return ptt_srpt;

    ptt_srpt->title = (dvd_ttu_t *)calloc(n_srpts, sizeof(dvd_ttu_t));
    if (!ptt_srpt->title) {
        free(ptt_srpt);
        return NULL;
    }

    for (uint16_t i = 0; i < n_srpts; i++) {
        uint32_t off = be32(data + base + 8U + (size_t)i * 4U);
        if ((size_t)off >= table_len) {
            continue;
        }
        size_t next_off = table_len;
        for (uint16_t j = 0; j < n_srpts; j++) {
            uint32_t cand = be32(data + base + 8U + (size_t)j * 4U);
            if ((size_t)cand > (size_t)off && (size_t)cand < next_off) {
                next_off = (size_t)cand;
            }
        }
        size_t n_ptts = (next_off - (size_t)off) / 4U;
        if (n_ptts > 0 && range_ok(size, base + (size_t)off, n_ptts * 4U)) {
            ptt_srpt->title[i].ptt = (dvd_ptt_info_t *)calloc(n_ptts, sizeof(dvd_ptt_info_t));
            if (ptt_srpt->title[i].ptt) {
                ptt_srpt->title[i].nr_of_ptts = (uint16_t)n_ptts;
                for (size_t k = 0; k < n_ptts; k++) {
                    const uint8_t *pe = data + base + (size_t)off + k * 4U;
                    ptt_srpt->title[i].ptt[k].pgcn = be16(pe);
                    ptt_srpt->title[i].ptt[k].pgn = be16(pe + 2);
                }
            }
        }
    }
    return ptt_srpt;
}

dvd_ifo_t *dvd_ifo_parse(const uint8_t *data, size_t size, int vts_nr) {
    if (!data || size < 0x400) {
        return NULL;
    }

    bool is_vmg = (vts_nr == 0);
    const char *expected_sig = is_vmg ? "DVDVIDEO-VMG" : "DVDVIDEO-VTS";
    if (memcmp(data, expected_sig, 12) != 0) {
        return NULL;
    }

    dvd_ifo_t *ifo = (dvd_ifo_t *)calloc(1, sizeof(dvd_ifo_t));
    if (!ifo) return NULL;

    if (is_vmg) {
        ifo->vmgi_mat = (dvd_vmgi_mat_t *)calloc(1, sizeof(dvd_vmgi_mat_t));
        if (!ifo->vmgi_mat) {
            dvd_ifo_close(ifo);
            return NULL;
        }
        uint16_t vmgm_vattr = be16(data + 0x100);
        ifo->vmgi_mat->vmgm_video_attr.video_format = (uint8_t)((vmgm_vattr >> 12) & 0x03);

        uint32_t fp_off = be32(data + 0x84);
        if (fp_off > 0) {
            ifo->first_play_pgc = parse_pgc(data, size, (size_t)fp_off);
        }

        uint32_t tt_sec = be32(data + 0xC4);
        size_t tt_base = 0;
        if (tt_sec > 0 && mul_u64_fits(tt_sec, DVD_IFO_SECTOR_SIZE, &tt_base)) {
            ifo->tt_srpt = parse_tt_srpt(data, size, tt_base);
        }

        uint32_t ut_sec = be32(data + 0xC8);
        size_t ut_base = 0;
        if (ut_sec > 0 && mul_u64_fits(ut_sec, DVD_IFO_SECTOR_SIZE, &ut_base)) {
            ifo->pgci_ut = parse_pgci_ut(data, size, ut_base);
        }
    } else {
        ifo->vtsi_mat = (dvd_vtsi_mat_t *)calloc(1, sizeof(dvd_vtsi_mat_t));
        if (!ifo->vtsi_mat) {
            dvd_ifo_close(ifo);
            return NULL;
        }

        uint16_t vtsm_vattr = be16(data + 0x100);
        uint16_t vts_vattr = be16(data + 0x200);
        ifo->vtsi_mat->vtsm_video_attr.video_format = (uint8_t)((vtsm_vattr >> 12) & 0x03);
        ifo->vtsi_mat->vts_video_attr.video_format = (uint8_t)((vts_vattr >> 12) & 0x03);

        uint16_t nr_aud = be16(data + 0x202);
        if (nr_aud > 8U) nr_aud = 8U;
        ifo->vtsi_mat->nr_of_vts_audio_streams = nr_aud;
        for (uint16_t i = 0; i < nr_aud; i++) {
            const uint8_t *ae = data + 0x204 + (size_t)i * 8U;
            ifo->vtsi_mat->vts_audio_attr[i].lang_code = be16(ae + 2);
            ifo->vtsi_mat->vts_audio_attr[i].lang_extension = ae[4];
            ifo->vtsi_mat->vts_audio_attr[i].code_extension = ae[5];
        }

        uint16_t nr_sub = be16(data + 0x254);
        if (nr_sub > 32U) nr_sub = 32U;
        ifo->vtsi_mat->nr_of_vts_subp_streams = nr_sub;
        for (uint16_t i = 0; i < nr_sub; i++) {
            const uint8_t *se = data + 0x256 + (size_t)i * 6U;
            ifo->vtsi_mat->vts_subp_attr[i].lang_code = be16(se + 2);
            ifo->vtsi_mat->vts_subp_attr[i].code_extension = se[5];
        }

        uint32_t ptt_sec = be32(data + 0xC8);
        size_t ptt_base = 0;
        if (ptt_sec > 0 && mul_u64_fits(ptt_sec, DVD_IFO_SECTOR_SIZE, &ptt_base)) {
            ifo->vts_ptt_srpt = parse_vts_ptt_srpt(data, size, ptt_base);
        }

        uint32_t pgcit_sec = be32(data + 0xCC);
        size_t pgcit_base = 0;
        if (pgcit_sec > 0 && mul_u64_fits(pgcit_sec, DVD_IFO_SECTOR_SIZE, &pgcit_base)) {
            ifo->vts_pgcit = parse_pgcit(data, size, pgcit_base);
        }

        uint32_t ut_sec = be32(data + 0xD0);
        size_t ut_base = 0;
        if (ut_sec > 0 && mul_u64_fits(ut_sec, DVD_IFO_SECTOR_SIZE, &ut_base)) {
            ifo->pgci_ut = parse_pgci_ut(data, size, ut_base);
        }
    }

    return ifo;
}

static uint8_t *read_disk_file_bytes(const char *path, size_t *out_size) {
    if (out_size) *out_size = 0;
    if (!path || !path[0]) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseeko(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    off_t fsz = ftello(f);
    if (fsz <= 0 || (uint64_t)fsz > DVD_IFO_MAX_BYTES) {
        fclose(f);
        return NULL;
    }
    if (fseeko(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)fsz);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)fsz, f) != (size_t)fsz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    if (out_size) *out_size = (size_t)fsz;
    return buf;
}

dvd_ifo_t *dvd_ifo_open(const tdvd_archive_t *archive, const char *dir_path, int vts_nr) {
    if (vts_nr < 0 || vts_nr >= 100) return NULL;

    char upper_ifo[64], upper_bup[64], lower_ifo[64], lower_bup[64];
    if (vts_nr == 0) {
        snprintf(upper_ifo, sizeof(upper_ifo), "VIDEO_TS.IFO");
        snprintf(upper_bup, sizeof(upper_bup), "VIDEO_TS.BUP");
        snprintf(lower_ifo, sizeof(lower_ifo), "video_ts.ifo");
        snprintf(lower_bup, sizeof(lower_bup), "video_ts.bup");
    } else {
        snprintf(upper_ifo, sizeof(upper_ifo), "VTS_%02d_0.IFO", vts_nr);
        snprintf(upper_bup, sizeof(upper_bup), "VTS_%02d_0.BUP", vts_nr);
        snprintf(lower_ifo, sizeof(lower_ifo), "vts_%02d_0.ifo", vts_nr);
        snprintf(lower_bup, sizeof(lower_bup), "vts_%02d_0.bup", vts_nr);
    }

    uint8_t *raw = NULL;
    size_t raw_size = 0;

    if (archive) {
        const char *cands[2] = { upper_ifo, upper_bup };
        for (int i = 0; i < 2; i++) {
            raw = tdvd_archive_read_entry(archive, cands[i], &raw_size);
            if (raw) {
                dvd_ifo_t *ifo = dvd_ifo_parse(raw, raw_size, vts_nr);
                free(raw);
                if (ifo) return ifo;
            }
        }
        return NULL;
    }

    if (!dir_path || !dir_path[0]) return NULL;
    const char *names[4] = { upper_ifo, upper_bup, lower_ifo, lower_bup };
    const char *subdirs[3] = { "", "/VIDEO_TS", "/video_ts" };
    char path[2048];
    for (int s = 0; s < 3; s++) {
        for (int n = 0; n < 4; n++) {
            snprintf(path, sizeof(path), "%s%s/%s", dir_path, subdirs[s], names[n]);
            raw = read_disk_file_bytes(path, &raw_size);
            if (raw) {
                dvd_ifo_t *ifo = dvd_ifo_parse(raw, raw_size, vts_nr);
                free(raw);
                if (ifo) return ifo;
            }
        }
    }
    return NULL;
}

void dvd_ifo_close(dvd_ifo_t *ifo) {
    if (!ifo) return;
    free(ifo->vmgi_mat);
    free(ifo->vtsi_mat);
    if (ifo->tt_srpt) {
        free(ifo->tt_srpt->title);
        free(ifo->tt_srpt);
    }
    if (ifo->vts_ptt_srpt) {
        if (ifo->vts_ptt_srpt->title) {
            for (uint16_t i = 0; i < ifo->vts_ptt_srpt->nr_of_srpts; i++) {
                free(ifo->vts_ptt_srpt->title[i].ptt);
            }
            free(ifo->vts_ptt_srpt->title);
        }
        free(ifo->vts_ptt_srpt);
    }
    free_pgc(ifo->first_play_pgc);
    free_pgci_ut(ifo->pgci_ut);
    free_pgcit(ifo->vts_pgcit);
    free(ifo);
}

bool dvd_nav_read_pci(dvd_pci_t *pci, const uint8_t *buf, size_t buf_len) {
    if (!pci || !buf || buf_len < DVD_PCI_MIN_SIZE) {
        return false;
    }
    memset(pci, 0, sizeof(*pci));

    pci->hli.hl_gi.hli_ss = (uint8_t)(be16(buf + 0x60) & 0x0003U);
    pci->hli.hl_gi.btn_ns = (uint8_t)(buf[0x72] & 0x3FU);
    if (pci->hli.hl_gi.btn_ns > 36) {
        pci->hli.hl_gi.btn_ns = 36;
    }
    pci->hli.hl_gi.fosl_btnn = (uint8_t)(buf[0x74] & 0x3FU);
    pci->hli.hl_gi.foac_btnn = (uint8_t)(buf[0x75] & 0x3FU);

    for (int c = 0; c < 3; c++) {
        for (int k = 0; k < 2; k++) {
            pci->hli.btn_colit.btn_coli[c][k] = be32(buf + 0x76 + (size_t)c * 8U + (size_t)k * 4U);
        }
    }

    for (int i = 0; i < 36; i++) {
        const uint8_t *e = buf + 0x8E + (size_t)i * 18U;
        dvd_btni_t *b = &pci->hli.btnit[i];
        b->btn_coln = (uint8_t)((e[0] >> 6) & 0x03U);
        b->x_start = (uint16_t)(((uint16_t)(e[0] & 0x3FU) << 4) | (e[1] >> 4));
        b->x_end = (uint16_t)(((uint16_t)(e[1] & 0x03U) << 8) | e[2]);
        b->auto_action_mode = (uint8_t)((e[3] >> 6) & 0x03U);
        b->y_start = (uint16_t)(((uint16_t)(e[3] & 0x3FU) << 4) | (e[4] >> 4));
        b->y_end = (uint16_t)(((uint16_t)(e[4] & 0x03U) << 8) | e[5]);
        b->up = (uint8_t)(e[6] & 0x3FU);
        b->down = (uint8_t)(e[7] & 0x3FU);
        b->left = (uint8_t)(e[8] & 0x3FU);
        b->right = (uint8_t)(e[9] & 0x3FU);
        memcpy(b->cmd.bytes, e + 10, 8U);
    }

    return true;
}
