#ifndef TINYDVD_DVD_IFO_H
#define TINYDVD_DVD_IFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tdvd_archive.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t bytes[8];
} dvd_vm_cmd_t;

typedef struct {
    uint8_t video_format; /* 0 = NTSC (525/60), 1 = PAL (625/50) */
} dvd_video_attr_t;

typedef struct {
    uint16_t lang_code;
    uint8_t  lang_extension;
    uint8_t  code_extension;
} dvd_audio_attr_t;

typedef struct {
    uint16_t lang_code;
    uint8_t  code_extension;
} dvd_subp_attr_t;

typedef struct {
    uint16_t nr_of_ptts;
    uint8_t  title_set_nr;
    uint8_t  vts_ttn;
} dvd_title_info_t;

typedef struct {
    uint16_t          nr_of_srpts;
    dvd_title_info_t *title;
} dvd_tt_srpt_t;

typedef struct {
    uint16_t pgcn;
    uint16_t pgn;
} dvd_ptt_info_t;

typedef struct {
    uint16_t        nr_of_ptts;
    dvd_ptt_info_t *ptt;
} dvd_ttu_t;

typedef struct {
    uint16_t   nr_of_srpts;
    dvd_ttu_t *title;
} dvd_vts_ptt_srpt_t;

typedef struct {
    uint16_t      nr_of_pre;
    uint16_t      nr_of_post;
    uint16_t      nr_of_cell;
    dvd_vm_cmd_t *pre_cmds;
    dvd_vm_cmd_t *post_cmds;
    dvd_vm_cmd_t *cell_cmds;
} dvd_pgc_command_tbl_t;

typedef struct {
    uint8_t  still_time;
    uint8_t  cell_cmd_nr;
    uint32_t first_sector;
    uint32_t last_sector;
} dvd_cell_playback_t;

typedef struct {
    uint16_t vob_id_nr;
    uint8_t  cell_nr;
} dvd_cell_position_t;

typedef struct {
    uint8_t                nr_of_programs;
    uint8_t                nr_of_cells;
    uint32_t               subp_control[32];
    uint16_t               next_pgc_nr;
    uint16_t               prev_pgc_nr;
    uint16_t               goup_pgc_nr;
    uint32_t               palette[16];
    dvd_pgc_command_tbl_t *command_tbl;
    uint8_t               *program_map;
    dvd_cell_playback_t   *cell_playback;
    dvd_cell_position_t   *cell_position;
} dvd_pgc_t;

typedef struct {
    uint8_t    entry_id;
    dvd_pgc_t *pgc;
} dvd_pgci_srp_t;

typedef struct {
    uint16_t        nr_of_pgci_srp;
    dvd_pgci_srp_t *pgci_srp;
} dvd_pgcit_t;

typedef struct {
    dvd_pgcit_t *pgcit;
} dvd_pgci_lu_t;

typedef struct {
    uint16_t       nr_of_lus;
    dvd_pgci_lu_t *lu;
} dvd_pgci_ut_t;

typedef struct {
    dvd_video_attr_t vmgm_video_attr;
} dvd_vmgi_mat_t;

typedef struct {
    dvd_video_attr_t vtsm_video_attr;
    dvd_video_attr_t vts_video_attr;
    uint16_t         nr_of_vts_audio_streams;
    dvd_audio_attr_t vts_audio_attr[8];
    uint16_t         nr_of_vts_subp_streams;
    dvd_subp_attr_t  vts_subp_attr[32];
} dvd_vtsi_mat_t;

typedef struct {
    dvd_vmgi_mat_t     *vmgi_mat;
    dvd_vtsi_mat_t     *vtsi_mat;
    dvd_tt_srpt_t      *tt_srpt;
    dvd_vts_ptt_srpt_t *vts_ptt_srpt;
    dvd_pgc_t          *first_play_pgc;
    dvd_pgci_ut_t      *pgci_ut;
    dvd_pgcit_t        *vts_pgcit;
} dvd_ifo_t;

typedef struct {
    uint8_t hli_ss;
    uint8_t btn_ns;
    uint8_t fosl_btnn;
    uint8_t foac_btnn;
} dvd_hl_gi_t;

typedef struct {
    uint32_t btn_coli[3][2];
} dvd_btn_colit_t;

typedef struct {
    uint8_t      btn_coln;
    uint16_t     x_start;
    uint16_t     x_end;
    uint8_t      auto_action_mode;
    uint16_t     y_start;
    uint16_t     y_end;
    uint8_t      up;
    uint8_t      down;
    uint8_t      left;
    uint8_t      right;
    dvd_vm_cmd_t cmd;
} dvd_btni_t;

typedef struct {
    dvd_hl_gi_t     hl_gi;
    dvd_btn_colit_t btn_colit;
    dvd_btni_t      btnit[36];
} dvd_hli_t;

typedef struct {
    dvd_hli_t hli;
} dvd_pci_t;

/**
 * Clean-room parser for DVD-Video VIDEO_TS.IFO (vts_nr == 0) and VTS_XX_0.IFO (vts_nr >= 1).
 * Validates magic header and bounds-checks every table offset and count.
 */
dvd_ifo_t *dvd_ifo_parse(const uint8_t *data, size_t size, int vts_nr);

/**
 * Open and parse VIDEO_TS.IFO (vts_nr == 0) or VTS_XX_0.IFO (vts_nr >= 1)
 * from a .tdvd archive (if archive != NULL) or an unpacked DVD directory on disk.
 */
dvd_ifo_t *dvd_ifo_open(const tdvd_archive_t *archive, const char *dir_path, int vts_nr);

/**
 * Release all memory associated with a parsed dvd_ifo_t handle.
 */
void dvd_ifo_close(dvd_ifo_t *ifo);

/**
 * Parse PCI / Highlight Information (HLI) from an NV_PCK PCI buffer (starts at sector + 45).
 */
bool dvd_nav_read_pci(dvd_pci_t *pci, const uint8_t *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* TINYDVD_DVD_IFO_H */
