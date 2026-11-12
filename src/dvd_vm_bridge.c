#include "dvd_vm_bridge.h"
#include "mvob_parser.h"
#include "tdvd_archive.h"
#include "spu_png.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_read.h>
#include <dvdread/ifo_types.h>
#include <dvdread/nav_read.h>
#include <dvdread/nav_types.h>

typedef enum {
    DVD_DOMAIN_FP = 0,
    DVD_DOMAIN_VMGM,
    DVD_DOMAIN_VTSM,
    DVD_DOMAIN_VTSTT,
    DVD_DOMAIN_EXIT
} dvd_domain_t;

typedef enum {
    ACT_NONE = 0,
    ACT_UP,
    ACT_DOWN,
    ACT_LEFT,
    ACT_RIGHT,
    ACT_ACTIVATE,
    ACT_MOUSE_MOVE,
    ACT_MOUSE_CLICK,
    ACT_MENU,
    ACT_ESCAPE,
    ACT_TOGGLE_PAUSE,
    ACT_SEEK_REL,
    ACT_SEEK_FRAC,
    ACT_NEXT_CHAPTER,
    ACT_PREV_CHAPTER,
    ACT_CYCLE_SUBS,
    ACT_CYCLE_AUDIO
} user_action_type_t;

typedef struct {
    uint32_t orig_lbn;       /* Original sector LBN from NV_PCK pci_gi.nv_pck_lbn */
    uint32_t orig_ea;        /* Original VOBU end address relative to orig_lbn */
    uint16_t vob_id;         /* DSI vobu_vob_idn */
    uint8_t  c_id;           /* DSI vobu_c_idn */
    uint8_t  file_idx;       /* Index into domain_mvob_files[] */
    uint64_t file_offset;    /* Byte offset of NV_PCK in that .mvob file */
    uint32_t sector_count;   /* Number of 2048-byte sectors in this compact .mvob VOBU */
    uint64_t pts_nanos;      /* Extracted from mvob_video_ref_t */
    uint32_t duration_nanos; /* Extracted from mvob_video_ref_t */
} mvob_vobu_entry_t;

typedef enum {
    VM_LINK_NONE = 0,
    VM_LINK_PGC_CHANGED, /* Domain / PGC / Cell was already updated */
    VM_LINK_NEXT_CELL,
    VM_LINK_PREV_CELL,
    VM_LINK_TOP_CELL,
    VM_LINK_TAIL_PGC,
    VM_LINK_TOP_PGC,
    VM_LINK_NEXT_PGC,
    VM_LINK_PREV_PGC,
    VM_LINK_GOUP_PGC,
    VM_LINK_EXIT
} vm_link_result_t;

struct dvd_vm_bridge {
    char *dvd_path;
    tdvd_archive_t *archive;
    player_adapter_t *player;
    spu_compositor_t *spu;

    /* IFO reader handles (libdvdread) */
    dvd_reader_t *reader;
    ifo_handle_t *vmgi;
    ifo_handle_t *vtsi[100];

    /* DVD Virtual Machine Registers */
    uint16_t gprm[16];
    uint16_t sprm[24];

    /* Resume state for CallSS / RSM */
    bool     rsm_valid;
    int      rsm_vts;
    int      rsm_pgc_nr;
    int      rsm_cell_nr;
    uint16_t rsm_sprm[5];

    /* Effective Root Menu tracking */
    bool     resolving_root_menu;
    int      effective_root_vts;
    int      effective_root_pgc_nr;

    /* Active VM execution location */
    dvd_domain_t domain;
    int          vts_nr;      /* 1..99 for VTSM/VTSTT, 0 for FP/VMGM */
    int          pgc_nr;      /* 1-based index into current PGC table */
    pgc_t       *current_pgc;
    int          cell_nr;     /* 1-based index into current_pgc->cell_playback */
    int          enter_depth; /* Guard against recursive Pre-command loops */

    /* Active domain compact .mvob index & companion MKV state */
    dvd_domain_t indexed_domain;
    int          indexed_vts;
    FILE        *domain_files[10];
    uint64_t     domain_file_base_offsets[10];
    uint64_t     domain_file_sizes[10];
    int          domain_file_count;
    mvob_vobu_entry_t *vobu_index;
    size_t       vobu_count;
    size_t       vobu_capacity;

    char         loaded_mkv_path[1024];
    char         current_video_name[64];
    int64_t      expected_next_pts_nanos;
    int64_t      current_pts_nanos;
    size_t       current_vobu_idx;
    int64_t      seek_target_vobu_idx;
    bool         user_paused;

    /* Active NV_PCK highlight state */
    bool         has_active_hli;
    hli_t        active_hli;

    /* Web streaming & event callback */
    dvd_vm_event_callback_t event_cb;
    void        *event_user_data;

    /* Threading & user input */
    pthread_t    thread;
    pthread_mutex_t lock;
    bool         running;
    bool         thread_started;

    user_action_type_t pending_action;
    int          action_x;
    int          action_y;
    double       action_frac;
};

static void emit_highlight_event(dvd_vm_bridge_t *b);

/* TINYDVD_TRACE=1 logs navigation to stderr: every PGC entered and every
 * navigation command run, with the general registers. For diagnosing discs
 * whose menus or play order misbehave. */
static int trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *v = getenv("TINYDVD_TRACE");
        enabled = (v && *v && strcmp(v, "0") != 0) ? 1 : 0;
    }
    return enabled;
}

#define VM_TRACE(...) do { if (trace_enabled()) fprintf(stderr, "[trace] " __VA_ARGS__); } while (0)

/* VOBU clock vs. player backend synchronization */
#define PLAYER_NOT_READY_HOLD_LIMIT_NANOS 10000000000ULL /* max wait for a load/seek to complete */
#define PLAYER_LAG_TOLERANCE_NANOS         1000000000ULL /* player may trail the VOBU end by this much */
#define PLAYER_LAG_GRACE_NANOS             5000000000ULL /* extra wait for a lagging player */

static uint64_t monotonic_nanos(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static bool file_exists(const char *path) {
    struct stat st;
    return (path && stat(path, &st) == 0);
}

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t read_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static ifo_handle_t *get_vtsi(dvd_vm_bridge_t *b, int vts_nr) {
    if (vts_nr < 1 || vts_nr >= 100) return NULL;
    if (!b->vtsi[vts_nr]) {
        b->vtsi[vts_nr] = ifoOpen(b->reader, vts_nr);
    }
    return b->vtsi[vts_nr];
}

static pgcit_t *get_menu_pgcit(dvd_vm_bridge_t *b, dvd_domain_t dom, int vts_nr) {
    pgci_ut_t *ut = NULL;
    if (dom == DVD_DOMAIN_VMGM) {
        ut = b->vmgi ? b->vmgi->pgci_ut : NULL;
    } else if (dom == DVD_DOMAIN_VTSM) {
        ifo_handle_t *vtsi = get_vtsi(b, vts_nr);
        ut = vtsi ? vtsi->pgci_ut : NULL;
    }
    if (!ut || ut->nr_of_lus == 0 || !ut->lu) return NULL;
    return ut->lu[0].pgcit;
}

static pgcit_t *get_title_pgcit(dvd_vm_bridge_t *b, int vts_nr) {
    ifo_handle_t *vtsi = get_vtsi(b, vts_nr);
    return vtsi ? vtsi->vts_pgcit : NULL;
}

static pgc_t *lookup_pgc(dvd_vm_bridge_t *b, dvd_domain_t dom, int vts_nr, int pgc_nr) {
    if (dom == DVD_DOMAIN_FP) {
        return b->vmgi ? b->vmgi->first_play_pgc : NULL;
    }
    pgcit_t *pgcit = (dom == DVD_DOMAIN_VTSTT) ? get_title_pgcit(b, vts_nr) : get_menu_pgcit(b, dom, vts_nr);
    if (!pgcit || pgc_nr < 1 || pgc_nr > pgcit->nr_of_pgci_srp) return NULL;
    return pgcit->pgci_srp[pgc_nr - 1].pgc;
}

static int find_menu_pgc_by_entry_id(dvd_vm_bridge_t *b, dvd_domain_t dom, int vts_nr, int menu_id) {
    pgcit_t *pgcit = get_menu_pgcit(b, dom, vts_nr);
    if (!pgcit) return 1;
    for (int i = 0; i < pgcit->nr_of_pgci_srp; i++) {
        if ((pgcit->pgci_srp[i].entry_id & 0x0F) == (menu_id & 0x0F)) {
            return i + 1;
        }
    }
    return 1;
}

/* Frame height of the current domain's video, from its IFO video attributes:
 * 576 for PAL, 480 for NTSC */
static int domain_frame_height(dvd_vm_bridge_t *b) {
    video_attr_t *attr = NULL;
    if (b->domain == DVD_DOMAIN_VTSM || b->domain == DVD_DOMAIN_VTSTT) {
        ifo_handle_t *vtsi = get_vtsi(b, b->vts_nr);
        if (vtsi && vtsi->vtsi_mat) {
            attr = (b->domain == DVD_DOMAIN_VTSM) ? &vtsi->vtsi_mat->vtsm_video_attr
                                                  : &vtsi->vtsi_mat->vts_video_attr;
        }
    } else if (b->vmgi && b->vmgi->vmgi_mat) {
        attr = &b->vmgi->vmgi_mat->vmgm_video_attr;
    }
    return (attr && attr->video_format == 1) ? SPU_PAL_HEIGHT : SPU_HEIGHT;
}

static void apply_pgc_clut_and_streams(dvd_vm_bridge_t *b, pgc_t *pgc) {
    if (!b || !pgc) return;
    if (b->spu) {
        spu_compositor_set_frame_height(b->spu, domain_frame_height(b));
        uint32_t clut[16];
        for (int i = 0; i < 16; i++) {
            clut[i] = pgc->palette[i];
        }
        spu_compositor_set_clut(b->spu, clut);

        /* In menus, always listen to substream 0x20 (Group 1 Widescreen).
         * In title domain, enable SPU substream only if SPRM[2] bit 6 (0x40) is set. */
        if (b->domain == DVD_DOMAIN_VTSM || b->domain == DVD_DOMAIN_VMGM) {
            spu_compositor_set_active_stream(b->spu, 0x20);
        } else {
            if (b->sprm[2] & 0x0040) {
                int track = b->sprm[2] & 0x1F;
                uint8_t sid = (uint8_t)(0x20 + track);
                if (pgc->subp_control[track] & 0x80000000U) {
                    uint8_t wide_id = (uint8_t)((pgc->subp_control[track] >> 16) & 0x1F);
                    sid = (uint8_t)(0x20 + wide_id);
                }
                spu_compositor_set_active_stream(b->spu, sid);
            } else {
                spu_compositor_set_active_stream(b->spu, 0x00);
            }
        }
    }
    if (b->player && b->domain == DVD_DOMAIN_VTSTT) {
        int aid = (int)(b->sprm[1] & 0x0F) + 1;
        player_adapter_set_audio_track(b->player, aid);
    }
}

/* =========================================================================
 * DVD Virtual Machine 8-Byte Instruction Decoder & Evaluator
 * ========================================================================= */

static uint16_t vm_read_reg(dvd_vm_bridge_t *b, uint8_t reg_code) {
    if (reg_code & 0x80) {
        int s = reg_code & 0x1F;
        if (s < 24) return b->sprm[s];
        return 0;
    }
    int g = reg_code & 0x0F;
    return b->gprm[g];
}

static void vm_write_gprm(dvd_vm_bridge_t *b, uint8_t reg_code, uint16_t val) {
    int g = reg_code & 0x0F;
    b->gprm[g] = val;
}

static bool vm_eval_compare(uint8_t cmp_op, uint16_t v1, uint16_t v2) {
    switch (cmp_op & 0x07) {
        case 0: return true;
        case 1: return (v1 & v2) != 0;
        case 2: return v1 == v2;
        case 3: return v1 != v2;
        case 4: return v1 >= v2;
        case 5: return v1 >  v2;
        case 6: return v1 <= v2;
        case 7: return v1 <  v2;
    }
    return true;
}

static void vm_exec_compute_op(dvd_vm_bridge_t *b, uint8_t op, uint8_t dst_reg, uint16_t src_val, uint8_t src_reg_if_swap) {
    uint16_t dst_val = vm_read_reg(b, dst_reg);
    uint32_t res = dst_val;
    switch (op & 0x0F) {
        case 0: return;
        case 1: res = src_val; break; /* Mov */
        case 2: /* Swp */
            vm_write_gprm(b, dst_reg, src_val);
            vm_write_gprm(b, src_reg_if_swap, dst_val);
            return;
        case 3: /* Add */
            res = (uint32_t)dst_val + (uint32_t)src_val;
            if (res > 0xFFFF) res = 0xFFFF;
            break;
        case 4: /* Sub */
            res = (dst_val >= src_val) ? (dst_val - src_val) : 0;
            break;
        case 5: /* Mul */
            res = (uint32_t)dst_val * (uint32_t)src_val;
            if (res > 0xFFFF) res = 0xFFFF;
            break;
        case 6: /* Div */
            if (src_val != 0) res = dst_val / src_val;
            break;
        case 7: /* Mod */
            if (src_val != 0) res = dst_val % src_val;
            break;
        case 8: /* Rnd */
            res = (src_val > 0) ? (uint16_t)(1 + (rand() % src_val)) : 1;
            break;
        case 9: /* And */
            res = dst_val & src_val;
            break;
        case 10: /* Or */
            res = dst_val | src_val;
            break;
        case 11: /* Xor */
            res = dst_val ^ src_val;
            break;
    }
    vm_write_gprm(b, dst_reg, (uint16_t)res);
}

static void vm_exec_system_set(dvd_vm_bridge_t *b, uint8_t sys_op, bool imm, const uint8_t *bytes) {
    switch (sys_op & 0x0F) {
        case 1: { /* SetSTN (audio, subp, angle) */
            uint8_t a_byte = bytes[3];
            uint8_t s_byte = bytes[4];
            uint8_t g_byte = bytes[5];
            if (!imm) {
                if (a_byte & 0x80) a_byte = 0x80 | (uint8_t)(vm_read_reg(b, a_byte & 0x0F) & 0x0F);
                else if ((a_byte & 0x0F) != 0) a_byte = 0x80 | (uint8_t)(vm_read_reg(b, a_byte & 0x0F) & 0x0F);
                if (s_byte & 0x80) s_byte = 0x80 | (uint8_t)(vm_read_reg(b, s_byte & 0x0F) & 0x7F);
                if (g_byte & 0x80) g_byte = 0x80 | (uint8_t)(vm_read_reg(b, g_byte & 0x0F) & 0x0F);
            }
            if (a_byte & 0x80) {
                b->sprm[1] = a_byte & 0x0F;
                if (b->player && b->domain == DVD_DOMAIN_VTSTT) {
                    player_adapter_set_audio_track(b->player, (int)b->sprm[1] + 1);
                }
            }
            if (s_byte & 0x80) {
                b->sprm[2] = s_byte & 0x7F;
                if (b->domain == DVD_DOMAIN_VTSTT && b->current_pgc) {
                    apply_pgc_clut_and_streams(b, b->current_pgc);
                }
            }
            if (g_byte & 0x80) {
                b->sprm[3] = g_byte & 0x0F;
            }
            break;
        }
        case 2: { /* SetNVTMR */
            uint16_t tmr = imm ? read_be16(bytes + 2) : vm_read_reg(b, bytes[3]);
            b->sprm[9] = tmr;
            b->sprm[10] = read_be16(bytes + 4);
            break;
        }
        case 3: { /* SetGPRMMD */
            uint16_t val = imm ? read_be16(bytes + 2) : vm_read_reg(b, bytes[3]);
            vm_write_gprm(b, bytes[5] & 0x0F, val);
            break;
        }
        case 6: { /* SetHL_BTNN */
            uint16_t btn_val = imm ? read_be16(bytes + 4) : vm_read_reg(b, bytes[5]);
            if (btn_val != 0) {
                b->sprm[8] = btn_val & 0xFC00;
            }
            break;
        }
        default:
            break;
    }
}

static bool vm_enter_pgc(dvd_vm_bridge_t *b, dvd_domain_t dom, int vts_nr, int pgc_nr, int start_cell);

static bool vm_jump_vts_ptt(dvd_vm_bridge_t *b, int vts_nr, int vts_ttn, int pttn) {
    ifo_handle_t *vtsi = get_vtsi(b, vts_nr);
    if (!vtsi || !vtsi->vts_ptt_srpt || vts_ttn < 1 || vts_ttn > vtsi->vts_ptt_srpt->nr_of_srpts) {
        return vm_enter_pgc(b, DVD_DOMAIN_VTSTT, vts_nr, 1, 1);
    }
    ttu_t *ttu = &vtsi->vts_ptt_srpt->title[vts_ttn - 1];
    if (pttn < 1 || pttn > ttu->nr_of_ptts) pttn = 1;
    int pgcn = ttu->ptt[pttn - 1].pgcn;
    int pgn  = ttu->ptt[pttn - 1].pgn;

    pgc_t *pgc = lookup_pgc(b, DVD_DOMAIN_VTSTT, vts_nr, pgcn);
    int cell_nr = 1;
    if (pgc && pgc->program_map && pgn >= 1 && pgn <= pgc->nr_of_programs) {
        cell_nr = pgc->program_map[pgn - 1];
    }

    b->sprm[5] = (uint16_t)vts_ttn;
    b->sprm[7] = (uint16_t)pttn;
    return vm_enter_pgc(b, DVD_DOMAIN_VTSTT, vts_nr, pgcn, cell_nr);
}

static bool vm_jump_global_tt(dvd_vm_bridge_t *b, int ttn) {
    if (!b->vmgi || !b->vmgi->tt_srpt || ttn < 1 || ttn > b->vmgi->tt_srpt->nr_of_srpts) {
        return false;
    }
    title_info_t *ti = &b->vmgi->tt_srpt->title[ttn - 1];
    b->sprm[4] = (uint16_t)ttn;
    return vm_jump_vts_ptt(b, ti->title_set_nr, ti->vts_ttn, 1);
}

static void vm_save_resume_state(dvd_vm_bridge_t *b, int rsm_cell) {
    if (b->domain == DVD_DOMAIN_VTSTT) {
        b->rsm_valid = true;
        b->rsm_vts = b->vts_nr;
        b->rsm_pgc_nr = b->pgc_nr;
        b->rsm_cell_nr = (rsm_cell > 0) ? rsm_cell : b->cell_nr;
        for (int i = 0; i < 5; i++) {
            b->rsm_sprm[i] = b->sprm[4 + i];
        }
    }
}

static vm_link_result_t vm_exec_link_sub(dvd_vm_bridge_t *b, uint8_t link_sub, const uint8_t *bytes) {
    uint16_t word3 = read_be16(bytes + 6);
    uint8_t button = (uint8_t)((word3 >> 10) & 0x3F);
    if (button > 0) {
        b->sprm[8] = (uint16_t)(button << 10);
    }

    switch (link_sub & 0x1F) {
        case 0x00: return VM_LINK_NONE;
        case 0x01: { /* LinkSIns (LinkNoLink, LinkTopC, LinkNextC, etc.) */
            uint8_t sub_code = bytes[7] & 0x1F;
            switch (sub_code) {
                case 0x00: return VM_LINK_NONE; /* LinkNoLink (button update only) */
                case 0x01: return VM_LINK_TOP_CELL;
                case 0x02: return VM_LINK_NEXT_CELL;
                case 0x03: return VM_LINK_PREV_CELL;
                case 0x05: return VM_LINK_TOP_CELL; /* LinkTopPG */
                case 0x06: return VM_LINK_NEXT_CELL; /* LinkNextPG */
                case 0x07: return VM_LINK_PREV_CELL; /* LinkPrevPG */
                case 0x09: return VM_LINK_TOP_PGC;
                case 0x0A: return VM_LINK_NEXT_PGC;
                case 0x0B: return VM_LINK_PREV_PGC;
                case 0x0C: return VM_LINK_GOUP_PGC;
                case 0x0D: return VM_LINK_TAIL_PGC;
                case 0x10: /* RSM */
                    if (b->rsm_valid) {
                        for (int i = 0; i < 5; i++) b->sprm[4 + i] = b->rsm_sprm[i];
                        vm_enter_pgc(b, DVD_DOMAIN_VTSTT, b->rsm_vts, b->rsm_pgc_nr, b->rsm_cell_nr);
                        return VM_LINK_PGC_CHANGED;
                    }
                    return VM_LINK_NONE;
                default:
                    return VM_LINK_NONE;
            }
        }
        case 0x04: { /* LinkPGCN */
            int target_pgc = word3 & 0x7FFF;
            vm_enter_pgc(b, b->domain, b->vts_nr, target_pgc, 1);
            return VM_LINK_PGC_CHANGED;
        }
        case 0x05: { /* LinkPTTN */
            int pttn = word3 & 0x03FF;
            vm_jump_vts_ptt(b, b->vts_nr, b->sprm[5] ? b->sprm[5] : 1, pttn);
            return VM_LINK_PGC_CHANGED;
        }
        case 0x06: { /* LinkPGN */
            int pgn = bytes[7] & 0x7F;
            int cell = 1;
            if (b->current_pgc && b->current_pgc->program_map && pgn >= 1 && pgn <= b->current_pgc->nr_of_programs) {
                cell = b->current_pgc->program_map[pgn - 1];
            }
            b->cell_nr = cell;
            b->expected_next_pts_nanos = -1;
            return VM_LINK_PGC_CHANGED;
        }
        case 0x07: { /* LinkCN */
            int cn = bytes[7];
            if (cn < 1) cn = 1;
            b->cell_nr = cn;
            b->expected_next_pts_nanos = -1;
            return VM_LINK_PGC_CHANGED;
        }
        default:
            return VM_LINK_NONE;
    }
}

static vm_link_result_t vm_exec_jump_sub(dvd_vm_bridge_t *b, uint8_t jump_sub, const uint8_t *bytes) {
    switch (jump_sub & 0x0F) {
        case 0x01: /* Exit */
            b->domain = DVD_DOMAIN_EXIT;
            return VM_LINK_EXIT;
        case 0x02: { /* JumpTT */
            int ttn = bytes[5] & 0x7F;
            vm_jump_global_tt(b, ttn);
            return VM_LINK_PGC_CHANGED;
        }
        case 0x03: { /* JumpVTS_TT */
            int vts_ttn = bytes[5] & 0x7F;
            vm_jump_vts_ptt(b, b->vts_nr, vts_ttn, 1);
            return VM_LINK_PGC_CHANGED;
        }
        case 0x05: { /* JumpVTS_PTT */
            int vts_ttn = bytes[5] & 0x7F;
            int pttn = ((bytes[2] & 0x03) << 8) | bytes[3];
            if (pttn < 1) pttn = 1;
            vm_jump_vts_ptt(b, b->vts_nr, vts_ttn, pttn);
            return VM_LINK_PGC_CHANGED;
        }
        case 0x06:   /* JumpSS */
        case 0x08: { /* CallSS */
            bool is_call = ((jump_sub & 0x0F) == 0x08);
            if (is_call) {
                vm_save_resume_state(b, bytes[4]);
            }
            uint8_t ss_type = (bytes[5] >> 6) & 0x03;
            uint8_t menu_id = bytes[5] & 0x0F;
            if (ss_type == 0) {
                /* First Play */
                vm_enter_pgc(b, DVD_DOMAIN_FP, 0, 1, 1);
            } else if (ss_type == 1) {
                /* VMGM menu by entry_id */
                int pgcn = find_menu_pgc_by_entry_id(b, DVD_DOMAIN_VMGM, 0, menu_id);
                vm_enter_pgc(b, DVD_DOMAIN_VMGM, 0, pgcn, 1);
            } else if (ss_type == 2) {
                /* VTSM menu: JumpSS has vts_nr in bytes[4] and ttn in bytes[3];
                 * CallSS is called from VTSTT of the same VTS and has rsm_cell in bytes[4]. */
                int vts = (!is_call && bytes[4] > 0) ? bytes[4] : b->vts_nr;
                if (vts < 1) vts = 1;
                int ttn = bytes[3];
                if (ttn > 0) b->sprm[5] = ttn;
                int pgcn = find_menu_pgc_by_entry_id(b, DVD_DOMAIN_VTSM, vts, menu_id);
                vm_enter_pgc(b, DVD_DOMAIN_VTSM, vts, pgcn, 1);
            } else if (ss_type == 3) {
                /* VMGM specific PGC number */
                int pgcn = ((bytes[2] & 0x7F) << 8) | bytes[3];
                if (pgcn < 1) pgcn = 1;
                vm_enter_pgc(b, DVD_DOMAIN_VMGM, 0, pgcn, 1);
            }
            return VM_LINK_PGC_CHANGED;
        }
        default:
            return VM_LINK_NONE;
    }
}

static vm_link_result_t vm_eval_cmd(dvd_vm_bridge_t *b, const vm_cmd_t *cmd, int *inout_pc, bool *out_break) {
    const uint8_t *bytes = cmd->bytes;
    uint8_t group = (bytes[0] >> 5) & 0x07;
    if (out_break) *out_break = false;

    if (group == 0) {
        /* Special Instructions: Nop, Goto, Break, SetTmpPML */
        bool cmp_imm = (bytes[1] & 0x80) != 0;
        uint8_t cmp_op = (bytes[1] >> 4) & 0x07;
        uint8_t sub_op = bytes[1] & 0x0F;
        if (cmp_op != 0) {
            uint16_t v1 = vm_read_reg(b, bytes[3]);
            uint16_t v2 = cmp_imm ? read_be16(bytes + 4) : vm_read_reg(b, bytes[5]);
            if (!vm_eval_compare(cmp_op, v1, v2)) {
                return VM_LINK_NONE;
            }
        }
        if (sub_op == 1) { /* Goto */
            if (inout_pc) *inout_pc = (int)bytes[7] - 1; /* 1-based line number */
        } else if (sub_op == 2) { /* Break */
            if (out_break) *out_break = true;
        } else if (sub_op == 3) { /* SetTmpPML */
            b->sprm[13] = bytes[6] & 0x0F;
            if (inout_pc) *inout_pc = (int)bytes[7] - 1;
        }
        return VM_LINK_NONE;
    }

    if (group == 1) {
        /* Link / Jump Instructions */
        uint8_t sub_group = (bytes[0] >> 4) & 0x01; /* 0 = Link, 1 = Jump */
        bool cmp_imm = (bytes[1] & 0x80) != 0;
        uint8_t cmp_op = (bytes[1] >> 4) & 0x07;
        uint8_t sub_op = bytes[1] & 0x0F;
        if (cmp_op != 0) {
            uint16_t v1, v2;
            if (sub_group == 1) {
                /* Format 2 (Jump instructions): comparison is strictly between two registers
                 * in byte 6 and byte 7. */
                v1 = vm_read_reg(b, bytes[6]);
                v2 = vm_read_reg(b, bytes[7]);
            } else {
                /* Format 1 (Link instructions): comparison is between byte 3 and byte 4-5 */
                v1 = vm_read_reg(b, bytes[3]);
                v2 = cmp_imm ? read_be16(bytes + 4) : vm_read_reg(b, bytes[5]);
            }
            if (!vm_eval_compare(cmp_op, v1, v2)) {
                return VM_LINK_NONE;
            }
        }
        if (sub_group == 0) {
            return vm_exec_link_sub(b, sub_op, bytes);
        } else {
            return vm_exec_jump_sub(b, sub_op, bytes);
        }
    }

    if (group == 2) {
        /* System Set Instructions (+ optional compare / link) */
        bool set_imm = (bytes[0] & 0x10) != 0;
        uint8_t sys_op = bytes[0] & 0x0F;
        uint8_t cmp_op = (bytes[1] >> 4) & 0x07;
        uint8_t link_sub = bytes[1] & 0x0F;

        if (cmp_op != 0) {
            /* Format 2 (System set instructions): comparison is strictly between two registers
             * in byte 6 and byte 7. */
            uint16_t v1 = vm_read_reg(b, bytes[6]);
            uint16_t v2 = vm_read_reg(b, bytes[7]);
            if (!vm_eval_compare(cmp_op, v1, v2)) {
                return VM_LINK_NONE;
            }
        }
        vm_exec_system_set(b, sys_op, set_imm, bytes);
        if (link_sub != 0) {
            return vm_exec_link_sub(b, link_sub, bytes);
        }
        return VM_LINK_NONE;
    }

    if (group == 3) {
        /* General Register Set Instructions (+ optional compare / link) */
        bool set_imm = (bytes[0] & 0x10) != 0;
        uint8_t compute_op = bytes[0] & 0x0F;
        bool cmp_imm = (bytes[1] & 0x80) != 0;
        uint8_t cmp_op = (bytes[1] >> 4) & 0x07;
        uint8_t link_sub = bytes[1] & 0x0F;

        if (cmp_op != 0) {
            uint8_t cmp_reg1 = bytes[2] ? bytes[2] : bytes[3];
            uint16_t v1 = vm_read_reg(b, cmp_reg1);
            uint16_t v2 = cmp_imm ? read_be16(bytes + 6) : vm_read_reg(b, bytes[7]);
            if (!vm_eval_compare(cmp_op, v1, v2)) {
                return VM_LINK_NONE;
            }
        }

        uint8_t dst_reg = bytes[3] & 0x0F;
        uint16_t src_val = set_imm ? read_be16(bytes + 4) : vm_read_reg(b, bytes[5]);
        vm_exec_compute_op(b, compute_op, dst_reg, src_val, bytes[5] & 0x0F);

        if (link_sub != 0) {
            return vm_exec_link_sub(b, link_sub, bytes);
        }
        return VM_LINK_NONE;
    }

    /* Groups 4, 5, 6: Combined Set + Compare + LinkSIns */
    bool set_imm = (bytes[0] & 0x10) != 0;
    uint8_t compute_op = bytes[0] & 0x0F;
    bool cmp_imm = (bytes[1] & 0x80) != 0;
    uint8_t cmp_op = (bytes[1] >> 4) & 0x07;
    uint8_t dst_reg = bytes[1] & 0x0F;
    uint16_t set_val = set_imm ? read_be16(bytes + 2) : vm_read_reg(b, bytes[3]);
    uint16_t cmp_v1 = (group == 4) ? set_val : vm_read_reg(b, dst_reg);
    uint16_t cmp_v2 = cmp_imm ? read_be16(bytes + 4) : vm_read_reg(b, bytes[5]);

    if (group == 4 || group == 5) {
        if (group == 4) {
            vm_exec_compute_op(b, compute_op, dst_reg, set_val, bytes[3] & 0x0F);
        }
        bool cond = (cmp_op == 0) || vm_eval_compare(cmp_op, cmp_v1, cmp_v2);
        if (cond) {
            if (group == 5) {
                vm_exec_compute_op(b, compute_op, dst_reg, set_val, bytes[3] & 0x0F);
            }
            return vm_exec_link_sub(b, 0x01, bytes);
        }
    } else if (group == 6) {
        bool cond = (cmp_op == 0) || vm_eval_compare(cmp_op, cmp_v1, cmp_v2);
        if (cond) {
            vm_exec_compute_op(b, compute_op, dst_reg, set_val, bytes[3] & 0x0F);
        }
        return vm_exec_link_sub(b, 0x01, bytes);
    }

    return VM_LINK_NONE;
}

static vm_link_result_t vm_run_cmd_list(dvd_vm_bridge_t *b, const vm_cmd_t *cmds, int num_cmds) {
    if (!cmds || num_cmds <= 0) return VM_LINK_NONE;
    int pc = 0;
    int safety = 0;
    while (pc >= 0 && pc < num_cmds && safety < 512) {
        safety++;
        bool do_break = false;
        int next_pc = pc + 1;
        if (trace_enabled()) {
            const uint8_t *c = cmds[pc].bytes;
            VM_TRACE("  %2d: %02x %02x %02x %02x %02x %02x %02x %02x   gprm",
                     pc + 1, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
            for (int g = 0; g < 16; g++) fprintf(stderr, " %u", b->gprm[g]);
            fprintf(stderr, "\n");
        }
        vm_link_result_t res = vm_eval_cmd(b, &cmds[pc], &next_pc, &do_break);
        if (res != VM_LINK_NONE) {
            return res;
        }
        if (do_break) {
            break;
        }
        pc = next_pc;
    }
    return VM_LINK_NONE;
}

/* =========================================================================
 * Compact .mvob Domain Indexer & Companion MKV Loader
 * ========================================================================= */

static void close_domain_files(dvd_vm_bridge_t *b) {
    for (int i = 0; i < b->domain_file_count; i++) {
        if (b->domain_files[i]) {
            fclose(b->domain_files[i]);
            b->domain_files[i] = NULL;
        }
    }
    b->domain_file_count = 0;
    b->vobu_count = 0;
}

static FILE *open_dvd_subfile(const char *dvd_path, const char *filename) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dvd_path, filename);
    FILE *f = fopen(path, "rb");
    if (f) return f;
    snprintf(path, sizeof(path), "%s/VIDEO_TS/%s", dvd_path, filename);
    return fopen(path, "rb");
}

static bool open_domain_subfile(dvd_vm_bridge_t *b, const char *filename) {
    if (!b || b->domain_file_count >= 10) return false;
    if (b->archive) {
        uint64_t base_off = 0, sz = 0;
        if (!tdvd_archive_find(b->archive, filename, &base_off, &sz)) {
            return false;
        }
        FILE *f = fopen(tdvd_archive_path(b->archive), "rb");
        if (!f) return false;
        int idx = b->domain_file_count++;
        b->domain_files[idx] = f;
        b->domain_file_base_offsets[idx] = base_off;
        b->domain_file_sizes[idx] = sz;
        return true;
    }

    FILE *f = open_dvd_subfile(b->dvd_path, filename);
    if (!f) return false;
    int idx = b->domain_file_count++;
    b->domain_files[idx] = f;
    b->domain_file_base_offsets[idx] = 0;
    b->domain_file_sizes[idx] = UINT64_MAX;
    return true;
}

static bool resolve_mkv_subfile(dvd_vm_bridge_t *b, const char *filename, char *out_buf, size_t out_len) {
    if (b && b->archive) {
        uint64_t mkv_off = 0, mkv_sz = 0;
        if (tdvd_archive_find(b->archive, filename, &mkv_off, &mkv_sz)) {
            snprintf(out_buf, out_len, "tdvd://%llu:%llu:%s",
                     (unsigned long long)mkv_off,
                     (unsigned long long)mkv_sz,
                     tdvd_archive_path(b->archive));
            return true;
        }
        return false;
    }
    const char *dvd_path = b->dvd_path;
    snprintf(out_buf, out_len, "%s/%s", dvd_path, filename);
    if (file_exists(out_buf)) return true;
    snprintf(out_buf, out_len, "%s/VIDEO_TS/%s", dvd_path, filename);
    if (file_exists(out_buf)) return true;
    return false;
}

static void ensure_domain_indexed(dvd_vm_bridge_t *b) {
    if (!b || (b->domain != DVD_DOMAIN_VMGM && b->domain != DVD_DOMAIN_VTSM && b->domain != DVD_DOMAIN_VTSTT)) {
        return;
    }

    if (b->indexed_domain == b->domain && b->indexed_vts == b->vts_nr && b->vobu_count > 0) {
        return;
    }

    close_domain_files(b);
    b->indexed_domain = b->domain;
    b->indexed_vts = b->vts_nr;

    /* 1. Locate .mvob (or .VOB) files for this domain */
    if (b->domain == DVD_DOMAIN_VMGM) {
        if (!open_domain_subfile(b, "VIDEO_TS.mvob")) {
            open_domain_subfile(b, "VIDEO_TS.VOB");
        }
    } else if (b->domain == DVD_DOMAIN_VTSM) {
        char name[64];
        snprintf(name, sizeof(name), "VTS_%02d_0.mvob", b->vts_nr);
        if (!open_domain_subfile(b, name)) {
            snprintf(name, sizeof(name), "VTS_%02d_0.VOB", b->vts_nr);
            open_domain_subfile(b, name);
        }
    } else if (b->domain == DVD_DOMAIN_VTSTT) {
        for (int part = 1; part <= 9; part++) {
            char name[64];
            snprintf(name, sizeof(name), "VTS_%02d_%d.mvob", b->vts_nr, part);
            if (!open_domain_subfile(b, name)) {
                snprintf(name, sizeof(name), "VTS_%02d_%d.VOB", b->vts_nr, part);
                if (!open_domain_subfile(b, name)) {
                    break;
                }
            }
        }
    }

    /* 2. Build the compact VOBU index across the domain's .mvob files */
    uint8_t sector[DVD_SECTOR_SIZE];
    mvob_vobu_entry_t *cur_entry = NULL;

    for (int fi = 0; fi < b->domain_file_count; fi++) {
        FILE *f = b->domain_files[fi];
        uint64_t base_off = b->domain_file_base_offsets[fi];
        uint64_t max_bytes = b->domain_file_sizes[fi];
        fseeko(f, (off_t)base_off, SEEK_SET);
        uint64_t offset = base_off;
        uint64_t bytes_read = 0;

        while (bytes_read + DVD_SECTOR_SIZE <= max_bytes &&
               fread(sector, 1, DVD_SECTOR_SIZE, f) == DVD_SECTOR_SIZE) {
            bool is_nv = (sector[0] == 0x00 && sector[1] == 0x00 && sector[2] == 0x01 && sector[3] == 0xBA &&
                          sector[38] == 0x00 && sector[39] == 0x00 && sector[40] == 0x01 && sector[41] == 0xBF &&
                          sector[44] == 0x00);
            if (is_nv) {
                if (b->vobu_count >= b->vobu_capacity) {
                    b->vobu_capacity = b->vobu_capacity ? (b->vobu_capacity * 2) : 2048;
                    b->vobu_index = (mvob_vobu_entry_t *)realloc(b->vobu_index, b->vobu_capacity * sizeof(mvob_vobu_entry_t));
                }
                cur_entry = &b->vobu_index[b->vobu_count++];
                memset(cur_entry, 0, sizeof(*cur_entry));
                cur_entry->orig_lbn = read_be32(sector + 45);
                if (sector[1024] == 0x00 && sector[1025] == 0x00 && sector[1026] == 0x01 && sector[1027] == 0xBF && sector[1030] == 0x01) {
                    cur_entry->orig_ea = read_be32(sector + 1039);
                    cur_entry->vob_id  = read_be16(sector + 1055);
                    cur_entry->c_id    = sector[1058];
                }
                cur_entry->file_idx = (uint8_t)fi;
                cur_entry->file_offset = offset;
                cur_entry->sector_count = 1;
                cur_entry->duration_nanos = 500000000U;
            } else if (cur_entry) {
                cur_entry->sector_count++;
                mvob_video_ref_t ref;
                if (mvob_parse_video_ref(sector, DVD_SECTOR_SIZE, &ref)) {
                    cur_entry->pts_nanos = ref.pts_nanos;
                    cur_entry->duration_nanos = ref.duration_nanos ? ref.duration_nanos : 500000000U;
                }
            }
            offset += DVD_SECTOR_SIZE;
            bytes_read += DVD_SECTOR_SIZE;
        }
    }

    /* 3. Load the companion MKV into mpv if it differs from currently loaded MKV */
    char mkv_name[64] = {0};
    if (b->domain == DVD_DOMAIN_VMGM) {
        snprintf(mkv_name, sizeof(mkv_name), "VIDEO_TS_VIDEO.mkv");
    } else if (b->domain == DVD_DOMAIN_VTSM) {
        snprintf(mkv_name, sizeof(mkv_name), "VTS_%02d_0_VIDEO.mkv", b->vts_nr);
    } else if (b->domain == DVD_DOMAIN_VTSTT) {
        snprintf(mkv_name, sizeof(mkv_name), "VTS_%02d_VIDEO.mkv", b->vts_nr);
    }

    char mkv_full[1024] = {0};
    if (mkv_name[0] && resolve_mkv_subfile(b, mkv_name, mkv_full, sizeof(mkv_full))) {
        if (strcmp(b->loaded_mkv_path, mkv_full) != 0) {
            strncpy(b->loaded_mkv_path, mkv_full, sizeof(b->loaded_mkv_path) - 1);
            strncpy(b->current_video_name, mkv_name, sizeof(b->current_video_name) - 1);
            b->expected_next_pts_nanos = -1;
            if (b->player) {
                fprintf(stderr, "[dvd_vm] Loading companion video: %s\n", mkv_full);
                player_adapter_load_file(b->player, mkv_full, mkv_name, NULL);
                if (b->domain == DVD_DOMAIN_VTSTT) {
                    player_adapter_set_audio_track(b->player, (int)(b->sprm[1] & 0x0F) + 1);
                }
            }
            if (b->event_cb) {
                dvd_vm_event_t ev;
                memset(&ev, 0, sizeof(ev));
                ev.type = DVD_VM_EVENT_LOAD_VIDEO;
                ev.video_name = b->current_video_name;
                ev.pts_nanos = 0;
                ev.paused = b->user_paused;
                b->event_cb(b->event_user_data, &ev);
            }
        }
    }
}

static bool vm_enter_pgc(dvd_vm_bridge_t *b, dvd_domain_t dom, int vts_nr, int pgc_nr, int start_cell) {
    if (!b || !b->running || b->enter_depth >= 16) {
        return false;
    }
    pgc_t *pgc = lookup_pgc(b, dom, vts_nr, pgc_nr);
    if (!pgc) {
        fprintf(stderr, "[dvd_vm] Warning: PGC not found (dom=%d, vts=%d, pgc=%d)\n", dom, vts_nr, pgc_nr);
        b->domain = DVD_DOMAIN_EXIT;
        return false;
    }

    /* Guard against a 0-cell dummy PGC whose Pre-command links directly back to itself */
    if (b->enter_depth > 0 && b->domain == dom && b->vts_nr == vts_nr &&
        b->pgc_nr == pgc_nr && pgc->nr_of_cells == 0) {
        return true;
    }

    VM_TRACE("enter %s vts=%d pgc=%d cell=%d (cells=%d, pre=%d, post=%d)\n",
             dom == DVD_DOMAIN_FP ? "FP" : dom == DVD_DOMAIN_VMGM ? "VMGM" :
             dom == DVD_DOMAIN_VTSM ? "VTSM" : dom == DVD_DOMAIN_VTSTT ? "VTSTT" : "?",
             vts_nr, pgc_nr, start_cell, pgc->nr_of_cells,
             pgc->command_tbl ? pgc->command_tbl->nr_of_pre : 0,
             pgc->command_tbl ? pgc->command_tbl->nr_of_post : 0);
    b->enter_depth++;

    /* If entering Root Menu entry point, begin resolving effective root menu */
    if (dom == DVD_DOMAIN_VTSM && vts_nr >= 1) {
        int entry_root = find_menu_pgc_by_entry_id(b, DVD_DOMAIN_VTSM, vts_nr, 3);
        if (pgc_nr == entry_root) {
            b->resolving_root_menu = true;
        }
    } else if (dom == DVD_DOMAIN_FP) {
        b->resolving_root_menu = true;
    }
    b->domain = dom;
    b->vts_nr = vts_nr;
    b->pgc_nr = pgc_nr;
    b->current_pgc = pgc;
    b->cell_nr = (start_cell >= 1) ? start_cell : 1;
    b->expected_next_pts_nanos = -1;
    b->seek_target_vobu_idx = -1;
    b->user_paused = false;
    b->sprm[6] = (uint16_t)pgc_nr;
    b->has_active_hli = false;

    if (b->spu) {
        spu_compositor_clear(b->spu);
    }
    emit_highlight_event(b);
    if (b->player) {
        player_adapter_on_state(b->player, b->domain, b->vts_nr, b->pgc_nr, b->cell_nr, b->sprm[7]);
    }

    /* Execute PGC Pre-commands on PGC entry before indexing/loading MKV */
    if (pgc->command_tbl && pgc->command_tbl->nr_of_pre > 0) {
        vm_link_result_t res = vm_run_cmd_list(b, pgc->command_tbl->pre_cmds, pgc->command_tbl->nr_of_pre);
        if (res == VM_LINK_PGC_CHANGED || res == VM_LINK_EXIT) {
            /* If a Pre-command jumped to a different PGC/domain or exited, that call already set up state */
            if (b->domain != dom || b->vts_nr != vts_nr || b->pgc_nr != pgc_nr || b->domain == DVD_DOMAIN_EXIT) {
                b->enter_depth--;
                return true;
            }
            /* Same PGC (e.g. LinkCN 1 / LinkPGN in Pre-commands): preserve chapter start_cell if > 1 */
            if (start_cell > 1) {
                b->cell_nr = start_cell;
            }
        } else if (res == VM_LINK_TAIL_PGC) {
            b->cell_nr = pgc->nr_of_cells + 1;
        } else if (start_cell > 1) {
            b->cell_nr = start_cell;
        }
    }

    if (b->current_pgc && b->current_pgc->nr_of_cells > 0 && b->cell_nr <= b->current_pgc->nr_of_cells) {
        if (b->domain == DVD_DOMAIN_VTSTT) {
            b->resolving_root_menu = false;
        }
        apply_pgc_clut_and_streams(b, b->current_pgc);
        ensure_domain_indexed(b);
    }

    b->enter_depth--;
    return true;
}

/* =========================================================================
 * Interactive Button Highlight & Selection Helpers
 * ========================================================================= */

static int get_selected_button_1based(dvd_vm_bridge_t *b) {
    if (!b->has_active_hli || b->active_hli.hl_gi.btn_ns == 0) return 0;
    int btn = (b->sprm[8] >> 10) & 0x3F;
    if (btn < 1 || btn > b->active_hli.hl_gi.btn_ns) {
        btn = b->active_hli.hl_gi.fosl_btnn ? b->active_hli.hl_gi.fosl_btnn : 1;
        if (btn < 1 || btn > b->active_hli.hl_gi.btn_ns) btn = 1;
        b->sprm[8] = (uint16_t)(btn << 10);
    }
    return btn;
}

static void emit_highlight_event(dvd_vm_bridge_t *b) {
    if (!b) return;
    dvd_vm_highlight_info_t hl;
    memset(&hl, 0, sizeof(hl));
    hl.frame_w = 720;
    hl.frame_h = domain_frame_height(b);
    hl.active_button = (b->domain != DVD_DOMAIN_VTSTT && b->has_active_hli && b->active_hli.hl_gi.btn_ns > 0) ? get_selected_button_1based(b) : 0;
    if (b->domain != DVD_DOMAIN_VTSTT && b->has_active_hli && b->active_hli.hl_gi.btn_ns > 0) {
        hl.button_count = b->active_hli.hl_gi.btn_ns;
        if (hl.button_count > 36) hl.button_count = 36;
        for (int i = 0; i < hl.button_count; i++) {
            hl.buttons[i].btn_id = i + 1;
            hl.buttons[i].x1 = b->active_hli.btnit[i].x_start;
            hl.buttons[i].y1 = b->active_hli.btnit[i].y_start;
            hl.buttons[i].x2 = b->active_hli.btnit[i].x_end;
            hl.buttons[i].y2 = b->active_hli.btnit[i].y_end;
        }
        if (b->spu) {
            int w = 0, h = 0;
            const uint32_t *rgba = spu_compositor_get_rgba(b->spu, &w, &h);
            if (rgba && w > 0 && h > 0) {
                hl.highlight_data_url = spu_rgba_to_data_url(rgba, w, h);
                hl.frame_w = w;
                hl.frame_h = h;
            }
        }
    }
    if (b->player) {
        player_adapter_on_highlight(b->player, &hl);
    }
    if (b->event_cb) {
        dvd_vm_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = DVD_VM_EVENT_HIGHLIGHT;
        ev.frame_w = hl.frame_w;
        ev.frame_h = hl.frame_h;
        ev.active_button = hl.active_button;
        ev.button_count = hl.button_count;
        for (int i = 0; i < hl.button_count; i++) {
            ev.buttons[i] = hl.buttons[i];
        }
        ev.highlight_data_url = hl.highlight_data_url;
        b->event_cb(b->event_user_data, &ev);
    }
    if (hl.highlight_data_url) {
        free(hl.highlight_data_url);
    }
}

static void vm_osd_message(dvd_vm_bridge_t *b, const char *msg) {
    if (!b || !msg) return;
    if (b->player) {
        player_adapter_osd_message(b->player, msg);
    }
    if (b->event_cb) {
        dvd_vm_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = DVD_VM_EVENT_OSD;
        ev.osd_message = msg;
        b->event_cb(b->event_user_data, &ev);
    }
}

static void refresh_button_highlight_on_spu(dvd_vm_bridge_t *b, bool action_palette) {
    if (!b) return;
    if (b->domain == DVD_DOMAIN_VTSTT || !b->has_active_hli || b->active_hli.hl_gi.btn_ns == 0) {
        if (b->spu) spu_compositor_clear_highlight(b->spu);
        emit_highlight_event(b);
        return;
    }

    int btn_idx = get_selected_button_1based(b);
    if (btn_idx < 1 || btn_idx > 36) {
        if (b->spu) spu_compositor_clear_highlight(b->spu);
        emit_highlight_event(b);
        return;
    }

    const btni_t *bt = &b->active_hli.btnit[btn_idx - 1];
    uint32_t palette = 0xF000F000U;
    if (bt->btn_coln >= 1 && bt->btn_coln <= 3) {
        palette = b->active_hli.btn_colit.btn_coli[bt->btn_coln - 1][action_palette ? 1 : 0];
    }

    if (b->spu) {
        spu_compositor_set_highlight(b->spu,
                                     (uint16_t)bt->x_start,
                                     (uint16_t)bt->y_start,
                                     (uint16_t)bt->x_end,
                                     (uint16_t)bt->y_end,
                                     palette);
    }
    emit_highlight_event(b);
}

/* =========================================================================
 * Timeline Seeking, Chapter Stepping, and Closed Caption / Track Cycling
 * ========================================================================= */

static bool cell_contains_vobu(const cell_playback_t *cell, const cell_position_t *pos, const mvob_vobu_entry_t *e) {
    if (cell->last_sector > 0) {
        return (e->orig_lbn >= cell->first_sector && e->orig_lbn <= cell->last_sector);
    }
    return (e->vob_id == pos->vob_id_nr && e->c_id == pos->cell_nr);
}

static void format_hhmmss(uint64_t total_sec, char *buf, size_t len) {
    unsigned int hrs  = (unsigned int)(total_sec / 3600ULL);
    unsigned int mins = (unsigned int)((total_sec % 3600ULL) / 60ULL);
    unsigned int secs = (unsigned int)(total_sec % 60ULL);
    if (hrs > 0) {
        snprintf(buf, len, "%02u:%02u:%02u", hrs, mins, secs);
    } else {
        snprintf(buf, len, "%02u:%02u", mins, secs);
    }
}

static void get_pgc_pts_bounds(dvd_vm_bridge_t *b, pgc_t *pgc, uint64_t *out_min, uint64_t *out_max) {
    uint64_t min_pts = UINT64_MAX;
    uint64_t max_pts = 0;
    if (b && pgc) {
        for (int c = 1; c <= pgc->nr_of_cells; c++) {
            const cell_playback_t *cell = &pgc->cell_playback[c - 1];
            const cell_position_t *pos  = &pgc->cell_position[c - 1];
            for (size_t i = 0; i < b->vobu_count; i++) {
                const mvob_vobu_entry_t *e = &b->vobu_index[i];
                if (!cell_contains_vobu(cell, pos, e)) continue;
                if (e->pts_nanos < min_pts) min_pts = e->pts_nanos;
                uint64_t end_pts = e->pts_nanos + e->duration_nanos;
                if (end_pts > max_pts) max_pts = end_pts;
            }
        }
    }
    if (min_pts == UINT64_MAX) min_pts = 0;
    if (max_pts < min_pts) max_pts = min_pts;
    if (out_min) *out_min = min_pts;
    if (out_max) *out_max = max_pts;
}

static int get_chapter_for_cell(pgc_t *pgc, int cell_nr, int *out_total_chapters) {
    if (!pgc) {
        if (out_total_chapters) *out_total_chapters = 1;
        return 1;
    }
    if (pgc->program_map && pgc->nr_of_programs > 0) {
        if (out_total_chapters) *out_total_chapters = pgc->nr_of_programs;
        int chap = 1;
        for (int p = 0; p < pgc->nr_of_programs; p++) {
            if (pgc->program_map[p] <= cell_nr) {
                chap = p + 1;
            }
        }
        return chap;
    }
    if (out_total_chapters) *out_total_chapters = pgc->nr_of_cells;
    return cell_nr;
}

static void show_timeline_osd(dvd_vm_bridge_t *b, uint64_t cur_pts, uint64_t min_pts, uint64_t max_pts, int cell_nr) {
    if (!b || !b->current_pgc) return;
    uint64_t rel_cur = (cur_pts >= min_pts) ? ((cur_pts - min_pts) / 1000000000ULL) : 0;
    uint64_t rel_tot = (max_pts >= min_pts) ? ((max_pts - min_pts) / 1000000000ULL) : 0;
    char cur_str[32], tot_str[32], osd[128];
    format_hhmmss(rel_cur, cur_str, sizeof(cur_str));
    format_hhmmss(rel_tot, tot_str, sizeof(tot_str));
    int total_chaps = 1;
    int chap = get_chapter_for_cell(b->current_pgc, cell_nr, &total_chaps);
    snprintf(osd, sizeof(osd), "Timeline: %s / %s  (Chapter %d/%d)", cur_str, tot_str, chap, total_chaps);
    vm_osd_message(b, osd);
}

static bool vm_seek_timeline(dvd_vm_bridge_t *b, bool is_relative, int delta_seconds, double fraction) {
    if (!b || !b->current_pgc || b->current_pgc->nr_of_cells == 0 || b->vobu_count == 0 || b->has_active_hli) {
        return false;
    }

    pgc_t *pgc = b->current_pgc;
    uint64_t min_pts = 0, max_pts = 0;
    get_pgc_pts_bounds(b, pgc, &min_pts, &max_pts);

    uint64_t cur_pts = min_pts;
    if (b->current_vobu_idx < b->vobu_count) {
        cur_pts = b->vobu_index[b->current_vobu_idx].pts_nanos;
    }

    uint64_t target_pts = cur_pts;
    if (is_relative) {
        int64_t t = (int64_t)cur_pts + (int64_t)delta_seconds * 1000000000LL;
        if (t < (int64_t)min_pts) t = (int64_t)min_pts;
        if (t > (int64_t)max_pts) t = (int64_t)max_pts;
        target_pts = (uint64_t)t;
    } else {
        if (fraction < 0.0) fraction = 0.0;
        if (fraction > 1.0) fraction = 1.0;
        uint64_t span = (max_pts > min_pts) ? (max_pts - min_pts) : 0;
        target_pts = min_pts + (uint64_t)((double)span * fraction);
    }

    int best_cell = b->cell_nr;
    size_t best_vobu = b->current_vobu_idx;
    uint64_t best_diff = UINT64_MAX;

    for (int c = 1; c <= pgc->nr_of_cells; c++) {
        const cell_playback_t *cell = &pgc->cell_playback[c - 1];
        const cell_position_t *pos  = &pgc->cell_position[c - 1];
        for (size_t i = 0; i < b->vobu_count; i++) {
            const mvob_vobu_entry_t *e = &b->vobu_index[i];
            if (!cell_contains_vobu(cell, pos, e)) continue;
            uint64_t diff = (e->pts_nanos >= target_pts)
                            ? (e->pts_nanos - target_pts)
                            : (target_pts - e->pts_nanos);
            if (diff < best_diff) {
                best_diff = diff;
                best_cell = c;
                best_vobu = i;
            }
        }
    }

    b->cell_nr = best_cell;
    b->seek_target_vobu_idx = (int64_t)best_vobu;
    b->current_vobu_idx = best_vobu;
    b->expected_next_pts_nanos = -1;
    if (b->spu) {
        spu_compositor_clear(b->spu);
    }
    emit_highlight_event(b);

    int total_chaps = 1;
    b->sprm[7] = (uint16_t)get_chapter_for_cell(pgc, best_cell, &total_chaps);
    show_timeline_osd(b, b->vobu_index[best_vobu].pts_nanos, min_pts, max_pts, best_cell);
    return true;
}

static bool vm_step_chapter(dvd_vm_bridge_t *b, int delta_chap) {
    if (!b || !b->current_pgc || b->current_pgc->nr_of_cells == 0 || b->has_active_hli) {
        return false;
    }

    pgc_t *pgc = b->current_pgc;
    int total_chaps = 1;
    int cur_chap = get_chapter_for_cell(pgc, b->cell_nr, &total_chaps);
    int next_chap = cur_chap + delta_chap;
    if (next_chap < 1) next_chap = 1;
    if (next_chap > total_chaps) next_chap = total_chaps;

    int target_cell = next_chap;
    if (pgc->program_map && pgc->nr_of_programs > 0 && next_chap <= pgc->nr_of_programs) {
        target_cell = pgc->program_map[next_chap - 1];
    }
    if (target_cell < 1) target_cell = 1;
    if (target_cell > pgc->nr_of_cells) target_cell = pgc->nr_of_cells;

    b->cell_nr = target_cell;
    b->seek_target_vobu_idx = -1;
    b->expected_next_pts_nanos = -1;
    b->sprm[7] = (uint16_t)next_chap;
    if (b->spu) {
        spu_compositor_clear(b->spu);
    }
    emit_highlight_event(b);

    /* Find first VOBU of target_cell for OSD timestamp */
    uint64_t min_pts = 0, max_pts = 0;
    get_pgc_pts_bounds(b, pgc, &min_pts, &max_pts);
    uint64_t cell_pts = min_pts;
    const cell_playback_t *cell = &pgc->cell_playback[target_cell - 1];
    const cell_position_t *pos  = &pgc->cell_position[target_cell - 1];
    for (size_t i = 0; i < b->vobu_count; i++) {
        if (cell_contains_vobu(cell, pos, &b->vobu_index[i])) {
            cell_pts = b->vobu_index[i].pts_nanos;
            b->current_vobu_idx = i;
            break;
        }
    }
    show_timeline_osd(b, cell_pts, min_pts, max_pts, target_cell);
    return true;
}

static const char *iso639_name(uint16_t lang_code, char *code_out) {
    char c1 = (char)((lang_code >> 8) & 0xFF);
    char c2 = (char)(lang_code & 0xFF);
    if (c1 >= 'A' && c1 <= 'Z') c1 = (char)(c1 + 32);
    if (c2 >= 'A' && c2 <= 'Z') c2 = (char)(c2 + 32);
    if (c1 > 32 && c2 > 32) {
        code_out[0] = c1;
        code_out[1] = c2;
        code_out[2] = '\0';
    } else {
        strcpy(code_out, "und");
    }
    if (strcmp(code_out, "en") == 0) return "English";
    if (strcmp(code_out, "fr") == 0) return "Francais";
    if (strcmp(code_out, "es") == 0) return "Espanol";
    if (strcmp(code_out, "de") == 0) return "Deutsch";
    if (strcmp(code_out, "it") == 0) return "Italiano";
    if (strcmp(code_out, "ja") == 0) return "Japanese";
    if (strcmp(code_out, "pt") == 0) return "Portugues";
    return "Track";
}

static void vm_cycle_subtitles(dvd_vm_bridge_t *b) {
    if (!b) return;
    ifo_handle_t *vtsi = (b->vts_nr >= 1) ? get_vtsi(b, b->vts_nr) : NULL;
    int nr_subp = (vtsi && vtsi->vtsi_mat) ? vtsi->vtsi_mat->nr_of_vts_subp_streams : 0;

    if (nr_subp <= 0) {
        /* Fallback for embedded EIA-608 / MKV closed captions when no DVD SPU streams exist */
        bool on = (b->sprm[2] & 0x0040) == 0;
        b->sprm[2] = on ? 0x0040 : 0x0000;
        if (b->player) {
            player_adapter_set_subtitle_track(b->player, on ? 1 : 0);
        }
        vm_osd_message(b, on ? "Closed Captions / Subtitles: On" : "Closed Captions / Subtitles: Off");
        return;
    }

    if ((b->sprm[2] & 0x0040) == 0) {
        b->sprm[2] = 0x0040; /* Turn on track 0 */
    } else {
        int next_track = (b->sprm[2] & 0x1F) + 1;
        if (next_track < nr_subp) {
            b->sprm[2] = (uint16_t)(0x0040 | next_track);
        } else {
            b->sprm[2] = 0x0000; /* Turn off */
        }
    }

    if (b->spu && b->domain == DVD_DOMAIN_VTSTT) {
        spu_compositor_clear(b->spu);
        emit_highlight_event(b);
    }
    if (b->current_pgc) {
        apply_pgc_clut_and_streams(b, b->current_pgc);
    }

    if ((b->sprm[2] & 0x0040) == 0) {
        vm_osd_message(b, "Closed Captions / Subtitles: Off");
        fprintf(stderr, "[dvd_vm] Closed Captions / Subtitles: Off\n");
    } else {
        int t = b->sprm[2] & 0x1F;
        subp_attr_t attr = vtsi->vtsi_mat->vts_subp_attr[t];
        char code[4];
        const char *lang_name = iso639_name(attr.lang_code, code);
        const char *ext = "";
        if (attr.code_extension == 5 || attr.code_extension == 6 || attr.code_extension == 7 ||
            (t == 0 && strcmp(code, "en") == 0)) {
            ext = " [CC / SDH]";
        } else if (attr.code_extension == 9) {
            ext = " [Forced]";
        }
        char osd[128];
        snprintf(osd, sizeof(osd), "Closed Captions / Subtitles: Track %d - %s (%s)%s",
                 t + 1, lang_name, code, ext);
        vm_osd_message(b, osd);
        fprintf(stderr, "[dvd_vm] %s\n", osd);
    }
}

static void vm_cycle_audio(dvd_vm_bridge_t *b) {
    if (!b) return;
    ifo_handle_t *vtsi = (b->vts_nr >= 1) ? get_vtsi(b, b->vts_nr) : NULL;
    int nr_aud = (vtsi && vtsi->vtsi_mat) ? vtsi->vtsi_mat->nr_of_vts_audio_streams : 1;
    if (nr_aud < 1) nr_aud = 1;

    int next_aud = ((int)(b->sprm[1] & 0x0F) + 1) % nr_aud;
    b->sprm[1] = (uint16_t)next_aud;

    if (b->player) {
        player_adapter_set_audio_track(b->player, next_aud + 1);
    }
    char osd[128];
    if (vtsi && vtsi->vtsi_mat && next_aud < vtsi->vtsi_mat->nr_of_vts_audio_streams) {
        audio_attr_t attr = vtsi->vtsi_mat->vts_audio_attr[next_aud];
        char code[4];
        const char *lang_name = iso639_name(attr.lang_code, code);
        snprintf(osd, sizeof(osd), "Audio: Track %d/%d - %s (%s)", next_aud + 1, nr_aud, lang_name, code);
    } else {
        snprintf(osd, sizeof(osd), "Audio: Track %d/%d", next_aud + 1, nr_aud);
    }
    vm_osd_message(b, osd);
    fprintf(stderr, "[dvd_vm] %s\n", osd);
}

static bool is_dummy_rsm_menu(const pgc_t *pgc) {
    if (!pgc) return true;
    if (pgc->nr_of_cells > 0) return false;
    /* PGC has 0 cells. Check its pre-commands */
    if (!pgc->command_tbl || pgc->command_tbl->nr_of_pre == 0) return true;
    /* If it has multiple pre-commands, it is a router PGC with logic, not a pure dummy */
    if (pgc->command_tbl->nr_of_pre > 1) return false;
    const uint8_t *last = pgc->command_tbl->pre_cmds[0].bytes;
    /* Group 1, Link, LinkSIns, RSM (subcode 0x10) */
    if ((last[0] >> 5) == 1 && ((last[0] >> 4) & 1) == 0 && (last[1] & 0x0F) == 1 && (last[7] & 0x1F) == 0x10) {
        return true;
    }
    return false;
}

static bool jump_from_vtstt_to_root_menu(dvd_vm_bridge_t *b) {
    int target_vts = (b->vts_nr >= 1) ? b->vts_nr : 1;
    if (b->domain == DVD_DOMAIN_VTSTT) {
        vm_save_resume_state(b, b->cell_nr);
    }
    int pgcn = find_menu_pgc_by_entry_id(b, DVD_DOMAIN_VTSM, target_vts, 3); /* Root Menu = 3 */
    pgc_t *menu_pgc = lookup_pgc(b, DVD_DOMAIN_VTSM, target_vts, pgcn);
    if (target_vts != 1 && is_dummy_rsm_menu(menu_pgc)) {
        int vts1_pgcn = find_menu_pgc_by_entry_id(b, DVD_DOMAIN_VTSM, 1, 3);
        pgc_t *vts1_menu = lookup_pgc(b, DVD_DOMAIN_VTSM, 1, vts1_pgcn);
        if (vts1_menu && !is_dummy_rsm_menu(vts1_menu)) {
            target_vts = 1;
            pgcn = vts1_pgcn;
            menu_pgc = vts1_menu;
        }
    }
    if (!menu_pgc || (menu_pgc->nr_of_cells == 0 && lookup_pgc(b, DVD_DOMAIN_VMGM, 0, 1) != NULL)) {
        vm_enter_pgc(b, DVD_DOMAIN_VMGM, 0, 1, 1);
    } else {
        vm_enter_pgc(b, DVD_DOMAIN_VTSM, target_vts, pgcn, 1);
    }
    return true;
}

/* Returns true if a button command or timeline seek changed PGC/Cell/Domain/VOBU */
static bool process_pending_user_action(dvd_vm_bridge_t *b) {
    pthread_mutex_lock(&b->lock);
    user_action_type_t act = b->pending_action;
    int ax = b->action_x;
    int ay = b->action_y;
    double afrac = b->action_frac;
    b->pending_action = ACT_NONE;
    pthread_mutex_unlock(&b->lock);

    if (act == ACT_NONE) return false;

    if (act == ACT_MENU) {
        return jump_from_vtstt_to_root_menu(b);
    }

    if (act == ACT_ESCAPE) {
        if (b->domain == DVD_DOMAIN_VTSTT) {
            return jump_from_vtstt_to_root_menu(b);
        } else if (b->domain == DVD_DOMAIN_VTSM) {
            int target_vts = (b->vts_nr >= 1) ? b->vts_nr : 1;
            int root_pgcn = (b->effective_root_pgc_nr > 0 && b->effective_root_vts == target_vts)
                            ? b->effective_root_pgc_nr
                            : find_menu_pgc_by_entry_id(b, DVD_DOMAIN_VTSM, target_vts, 3);
            if (b->current_pgc && b->current_pgc->goup_pgc_nr > 0 &&
                b->current_pgc->goup_pgc_nr != b->pgc_nr) {
                vm_enter_pgc(b, DVD_DOMAIN_VTSM, target_vts, b->current_pgc->goup_pgc_nr, 1);
                return true;
            } else if (b->pgc_nr != root_pgcn) {
                int entry_root = find_menu_pgc_by_entry_id(b, DVD_DOMAIN_VTSM, target_vts, 3);
                b->resolving_root_menu = true;
                vm_enter_pgc(b, DVD_DOMAIN_VTSM, target_vts, entry_root > 0 ? entry_root : root_pgcn, 1);
                return true;
            } else {
                b->domain = DVD_DOMAIN_EXIT;
                b->running = false;
                return true;
            }
        } else {
            b->domain = DVD_DOMAIN_EXIT;
            b->running = false;
            return true;
        }
    }

    if (act == ACT_CYCLE_SUBS) {
        vm_cycle_subtitles(b);
        return false;
    }

    if (act == ACT_CYCLE_AUDIO) {
        vm_cycle_audio(b);
        return false;
    }

    if (act == ACT_TOGGLE_PAUSE) {
        if (b->has_active_hli && b->active_hli.hl_gi.btn_ns > 0) {
            act = ACT_ACTIVATE;
        } else {
            b->user_paused = !b->user_paused;
            if (b->player) {
                player_adapter_set_paused(b->player, b->user_paused);
            }
            vm_osd_message(b, b->user_paused ? "Paused" : "Playing");
            if (b->event_cb) {
                dvd_vm_event_t ev;
                memset(&ev, 0, sizeof(ev));
                ev.type = DVD_VM_EVENT_PAUSE;
                ev.paused = b->user_paused;
                b->event_cb(b->event_user_data, &ev);
            }
            return false;
        }
    }

    if (act == ACT_SEEK_REL) {
        return vm_seek_timeline(b, true, ax, 0.0);
    }
    if (act == ACT_SEEK_FRAC) {
        return vm_seek_timeline(b, false, 0, afrac);
    }
    if (act == ACT_NEXT_CHAPTER) {
        return vm_step_chapter(b, +1);
    }
    if (act == ACT_PREV_CHAPTER) {
        return vm_step_chapter(b, -1);
    }

    if (!b->has_active_hli || b->active_hli.hl_gi.btn_ns == 0) {
        /* When playing a clip/movie with no interactive menu buttons active,
         * Arrow keys seek along the clip timeline (-10s/+10s, -60s/+60s)! */
        if (act == ACT_LEFT)  return vm_seek_timeline(b, true, -10, 0.0);
        if (act == ACT_RIGHT) return vm_seek_timeline(b, true, +10, 0.0);
        if (act == ACT_DOWN)  return vm_seek_timeline(b, true, -60, 0.0);
        if (act == ACT_UP)    return vm_seek_timeline(b, true, +60, 0.0);
        return false;
    }

    int cur_btn = get_selected_button_1based(b);
    const btni_t *bt = &b->active_hli.btnit[cur_btn - 1];
    int next_btn = cur_btn;
    bool trigger_activate = false;

    int n_btns = b->active_hli.hl_gi.btn_ns;
    switch (act) {
        case ACT_UP:
            if (bt->up >= 1 && bt->up <= n_btns && bt->up != cur_btn) next_btn = bt->up;
            else if (bt->left >= 1 && bt->left <= n_btns) next_btn = bt->left;
            break;
        case ACT_DOWN:
            if (bt->down >= 1 && bt->down <= n_btns && bt->down != cur_btn) next_btn = bt->down;
            else if (bt->right >= 1 && bt->right <= n_btns) next_btn = bt->right;
            break;
        case ACT_LEFT:
            if (bt->left >= 1 && bt->left <= n_btns && bt->left != cur_btn) next_btn = bt->left;
            else if (bt->up >= 1 && bt->up <= n_btns) next_btn = bt->up;
            break;
        case ACT_RIGHT:
            if (bt->right >= 1 && bt->right <= n_btns && bt->right != cur_btn) next_btn = bt->right;
            else if (bt->down >= 1 && bt->down <= n_btns) next_btn = bt->down;
            break;
        case ACT_ACTIVATE:
            trigger_activate = true;
            break;
        case ACT_MOUSE_MOVE:
        case ACT_MOUSE_CLICK: {
            for (int i = 0; i < b->active_hli.hl_gi.btn_ns && i < 36; i++) {
                const btni_t *cand = &b->active_hli.btnit[i];
                if (ax >= (int)cand->x_start && ax <= (int)cand->x_end &&
                    ay >= (int)cand->y_start && ay <= (int)cand->y_end) {
                    next_btn = i + 1;
                    if (act == ACT_MOUSE_CLICK) {
                        trigger_activate = true;
                    }
                    break;
                }
            }
            break;
        }
        default:
            break;
    }

    if (next_btn != cur_btn) {
        b->sprm[8] = (uint16_t)(next_btn << 10);
        bt = &b->active_hli.btnit[next_btn - 1];
        refresh_button_highlight_on_spu(b, false);
        if (bt->auto_action_mode) {
            trigger_activate = true;
        }
    }

    if (trigger_activate) {
        b->resolving_root_menu = false;
        refresh_button_highlight_on_spu(b, true);
        vm_link_result_t res = vm_eval_cmd(b, &bt->cmd, NULL, NULL);
        if (res != VM_LINK_NONE) {
            if (res == VM_LINK_NEXT_CELL) b->cell_nr++;
            else if (res == VM_LINK_PREV_CELL && b->cell_nr > 1) b->cell_nr--;
            else if (res == VM_LINK_TAIL_PGC && b->current_pgc) b->cell_nr = b->current_pgc->nr_of_cells + 1;
            else if (res == VM_LINK_TOP_PGC) vm_enter_pgc(b, b->domain, b->vts_nr, b->pgc_nr, 1);
            else if (res == VM_LINK_NEXT_PGC && b->current_pgc && b->current_pgc->next_pgc_nr)
                vm_enter_pgc(b, b->domain, b->vts_nr, b->current_pgc->next_pgc_nr, 1);
            else if (res == VM_LINK_PREV_PGC && b->current_pgc && b->current_pgc->prev_pgc_nr)
                vm_enter_pgc(b, b->domain, b->vts_nr, b->current_pgc->prev_pgc_nr, 1);
            else if (res == VM_LINK_GOUP_PGC && b->current_pgc && b->current_pgc->goup_pgc_nr)
                vm_enter_pgc(b, b->domain, b->vts_nr, b->current_pgc->goup_pgc_nr, 1);
            return true;
        }
    }

    return false;
}

/* =========================================================================
 * Cell & VOBU Playback Loop (Compact .mvob Reader + Smooth MPV Sync)
 * ========================================================================= */

static bool play_vobu_entry(dvd_vm_bridge_t *b, const mvob_vobu_entry_t *entry, bool is_static_still_cell) {
    if (!b || !entry || entry->file_idx >= b->domain_file_count) return false;
    FILE *f = b->domain_files[entry->file_idx];
    if (!f) return false;

    fseeko(f, (off_t)entry->file_offset, SEEK_SET);
    uint8_t sector[DVD_SECTOR_SIZE];

    for (uint32_t s = 0; s < entry->sector_count; s++) {
        if (fread(sector, 1, DVD_SECTOR_SIZE, f) != DVD_SECTOR_SIZE) break;

        if (s == 0) {
            /* First sector of VOBU is NV_PCK: parse PCI & DSI via libdvdread */
            pci_t pci;
            navRead_PCI(&pci, sector + 45);
            if (pci.hli.hl_gi.hli_ss == 1) {
                b->active_hli = pci.hli;
                b->has_active_hli = (pci.hli.hl_gi.btn_ns > 0);
                if (pci.hli.hl_gi.fosl_btnn > 0) {
                    b->sprm[8] = (uint16_t)(pci.hli.hl_gi.fosl_btnn << 10);
                }
            } else if (pci.hli.hl_gi.hli_ss == 2) {
                if (!b->has_active_hli && pci.hli.hl_gi.btn_ns > 0) {
                    b->active_hli = pci.hli;
                    b->has_active_hli = true;
                }
            } else if (pci.hli.hl_gi.hli_ss == 0) {
                b->has_active_hli = false;
                if (b->spu) spu_compositor_clear_highlight(b->spu);
                emit_highlight_event(b);
            }
        } else {
            /* Feed SPU sectors to compositor */
            if (b->spu) {
                spu_compositor_feed_sector(b->spu, sector, DVD_SECTOR_SIZE);
            }
        }
    }

    if (b->has_active_hli) {
        if (b->resolving_root_menu && b->active_hli.hl_gi.btn_ns > 0 &&
            (b->domain == DVD_DOMAIN_VTSM || b->domain == DVD_DOMAIN_VMGM)) {
            b->effective_root_vts = b->vts_nr;
            b->effective_root_pgc_nr = b->pgc_nr;
        }
        refresh_button_highlight_on_spu(b, false);
    }

    if (is_static_still_cell) {
        b->current_pts_nanos = (int64_t)entry->pts_nanos;
        b->user_paused = true;
        if (b->player) {
            /* A VOBU's start time can fall slightly before its first picture
             * (eulogy's scene menu: VOBU 21.755s, first frame 21.797s). mpv's
             * exact seek shows the first frame at/after the target, but a
             * browser <video> shows the frame on screen AT the target: the
             * previous menu's last frame, frozen. Aim inside the VOBU so every
             * backend lands on the still's own picture. */
            uint64_t into = entry->duration_nanos ? entry->duration_nanos / 2 : 100000000ULL;
            if (into > 250000000ULL) into = 250000000ULL;
            uint64_t target = entry->pts_nanos + into;
            fprintf(stderr, "[dvd_vm] Seeking to %.3fs (Still Cell %d, LBN %u, VOBU starts %.3fs)\n",
                    (double)target / 1e9, b->cell_nr, entry->orig_lbn, (double)entry->pts_nanos / 1e9);
            player_adapter_seek_exact(b->player, target, true);
        }
        if (b->event_cb) {
            dvd_vm_event_t ev_seek;
            memset(&ev_seek, 0, sizeof(ev_seek));
            ev_seek.type = DVD_VM_EVENT_SEEK;
            ev_seek.pts_nanos = entry->pts_nanos;
            ev_seek.paused = true;
            b->event_cb(b->event_user_data, &ev_seek);

            dvd_vm_event_t ev_pause;
            memset(&ev_pause, 0, sizeof(ev_pause));
            ev_pause.type = DVD_VM_EVENT_PAUSE;
            ev_pause.paused = true;
            b->event_cb(b->event_user_data, &ev_pause);
        }
        b->expected_next_pts_nanos = -1;
        return false;
    }

    /* Check if this VOBU's timestamp is consecutive with the previous VOBU!
     * If consecutive (within 1.5s tolerance for GOP variations), let playback continue smoothly without seeking! */
    bool need_seek = true;
    if (b->expected_next_pts_nanos >= 0) {
        int64_t diff = (int64_t)entry->pts_nanos - b->expected_next_pts_nanos;
        if (diff < 0) diff = -diff;
        if (diff <= 1500000000LL) {
            need_seek = false;
        }
    }

    /* Self-healing check: if player's actual playback clock has drifted > 5.0s from this VOBU, force a seek */
    if (!need_seek && b->player) {
        int64_t cur_pts = (int64_t)player_adapter_get_pts_nanos(b->player);
        if (cur_pts > 0) {
            int64_t clock_diff = (int64_t)entry->pts_nanos - cur_pts;
            if (clock_diff < 0) clock_diff = -clock_diff;
            if (clock_diff > 5000000000LL) {
                need_seek = true;
            }
        }
    }

    b->current_pts_nanos = (int64_t)entry->pts_nanos;
    if (need_seek) {
        if (b->player) {
            fprintf(stderr, "[dvd_vm] Seeking to %.3fs (Cell %d, LBN %u)\n",
                    (double)entry->pts_nanos / 1e9, b->cell_nr, entry->orig_lbn);
            player_adapter_seek_exact(b->player, entry->pts_nanos, b->user_paused);
        }
        if (b->event_cb) {
            dvd_vm_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = DVD_VM_EVENT_SEEK;
            ev.pts_nanos = entry->pts_nanos;
            ev.paused = b->user_paused;
            b->event_cb(b->event_user_data, &ev);
        }
    } else if (b->player && !b->user_paused && player_adapter_is_paused(b->player)) {
        player_adapter_set_paused(b->player, false);
    }

    b->expected_next_pts_nanos = (int64_t)entry->pts_nanos + (int64_t)entry->duration_nanos;

    /* Wait for VOBU duration while polling user button navigation */
    uint64_t start_wall = monotonic_nanos();
    uint64_t target_dur = entry->duration_nanos ? entry->duration_nanos : 500000000ULL;
    if (getenv("TINYDVD_FAST_TEST") != NULL) {
        target_dur = 0;
    }
    uint64_t vobu_end_pts = entry->pts_nanos + target_dur;
    uint64_t hold_start = 0;   /* when the player stopped being ready */
    uint64_t wait_since = 0;   /* when the current hold began (for diagnostics) */
    bool wait_logged = false;

    while (b->running) {
        if (process_pending_user_action(b)) {
            return true; /* User activated a button, menu transition, or timeline seek */
        }

        if (b->user_paused && !b->has_active_hli) {
            start_wall = monotonic_nanos();
            usleep(15000);
            continue;
        }

        if (getenv("TINYDVD_FAST_TEST") != NULL) {
            break;
        }

        if (b->player) {
            uint64_t now = monotonic_nanos();
            /* The backend played to the end of the file: nothing more will come
             * from this VOBU (mpv keep-open and <video> both pause at EOF) */
            if (player_adapter_is_ended(b->player)) {
                break;
            }
            /* Hold the VM clock while the backend is loading, seeking, buffering
             * or paused (e.g. browser autoplay blocked), so the navigation timeline
             * does not run ahead of the picture. A load/seek that never completes
             * only holds for a bounded time. Menus with buttons keep their own
             * timing because they are driven by user input. */
            if (!b->has_active_hli) {
                bool not_ready = !player_adapter_is_ready(b->player);
                bool paused = player_adapter_is_paused(b->player);
                if (not_ready && hold_start == 0) hold_start = now;
                if (!not_ready) hold_start = 0;
                bool hold_expired = hold_start != 0 && (now - hold_start) >= PLAYER_NOT_READY_HOLD_LIMIT_NANOS;
                if ((not_ready && !hold_expired) || (paused && !not_ready)) {
                    if (wait_since == 0) wait_since = now;
                    if (!wait_logged && now - wait_since >= 3000000000ULL) {
                        wait_logged = true;
                        fprintf(stderr, "[dvd_vm] Waiting on player for 3s+ at %.3fs: %s\n",
                                (double)entry->pts_nanos / 1e9,
                                not_ready ? "load/seek not complete" : "player reports paused");
                    }
                    start_wall = now;
                    usleep(15000);
                    continue;
                }
                wait_since = 0;
            }

            uint64_t elapsed_wall = now - start_wall;
            uint64_t cur_pts = player_adapter_get_pts_nanos(b->player);
            /* Player clock reached the end of this VOBU */
            if (elapsed_wall >= 80000000ULL &&
                cur_pts >= vobu_end_pts && cur_pts <= vobu_end_pts + 1500000000ULL) {
                break;
            }
            /* Wall clock elapsed: advance unless the player is lagging well behind
             * (stalled stream), in which case give it a bounded grace period */
            if (elapsed_wall >= target_dur) {
                bool lagging = cur_pts + PLAYER_LAG_TOLERANCE_NANOS < vobu_end_pts;
                if (!lagging || elapsed_wall >= target_dur + PLAYER_LAG_GRACE_NANOS) {
                    break;
                }
            }
        } else {
            uint64_t elapsed_wall = monotonic_nanos() - start_wall;
            if (elapsed_wall >= target_dur) {
                break;
            }
        }

        usleep(10000); /* 10ms */
    }

    return false;
}

static void vm_finish_pgc(dvd_vm_bridge_t *b) {
    pgc_t *pgc = b->current_pgc;
    if (!pgc) {
        b->domain = DVD_DOMAIN_EXIT;
        return;
    }

    if (pgc->command_tbl && pgc->command_tbl->nr_of_post > 0) {
        vm_link_result_t res = vm_run_cmd_list(b, pgc->command_tbl->post_cmds, pgc->command_tbl->nr_of_post);
        if (res == VM_LINK_PGC_CHANGED || res == VM_LINK_EXIT) {
            return;
        }
        if (res == VM_LINK_TOP_PGC) {
            vm_enter_pgc(b, b->domain, b->vts_nr, b->pgc_nr, 1);
            return;
        }
    }

    if (pgc->next_pgc_nr > 0) {
        vm_enter_pgc(b, b->domain, b->vts_nr, pgc->next_pgc_nr, 1);
        return;
    }

    /* If a menu PGC finishes with no post-link and no next_pgc_nr, loop the menu PGC */
    if (b->domain == DVD_DOMAIN_VTSM || b->domain == DVD_DOMAIN_VMGM) {
        vm_enter_pgc(b, b->domain, b->vts_nr, b->pgc_nr, 1);
        return;
    }

    b->domain = DVD_DOMAIN_EXIT;
}

static void *vm_thread_func(void *arg) {
    dvd_vm_bridge_t *b = (dvd_vm_bridge_t *)arg;

    /* Start DVD VM at First Play PGC */
    vm_enter_pgc(b, DVD_DOMAIN_FP, 0, 1, 1);

    while (b->running && b->domain != DVD_DOMAIN_EXIT) {
        pgc_t *pgc = b->current_pgc;
        if (!pgc || pgc->nr_of_cells == 0 || b->cell_nr > pgc->nr_of_cells) {
            vm_finish_pgc(b);
            continue;
        }

        int cur_cell_nr = b->cell_nr;
        cell_playback_t *cell = &pgc->cell_playback[cur_cell_nr - 1];
        cell_position_t *pos  = &pgc->cell_position[cur_cell_nr - 1];

        /* Update SPRM[7] (PTT / Chapter number) if in VTSTT */
        if (b->domain == DVD_DOMAIN_VTSTT && pgc->program_map) {
            for (int p = 0; p < pgc->nr_of_programs; p++) {
                if (pgc->program_map[p] == cur_cell_nr) {
                    b->sprm[7] = (uint16_t)(p + 1);
                    break;
                }
            }
        }
        if (b->player) {
            player_adapter_on_state(b->player, b->domain, b->vts_nr, b->pgc_nr, cur_cell_nr, b->sprm[7]);
        }

        /* Count how many VOBUs belong to this Cell */
        size_t cell_vobu_count = 0;
        for (size_t i = 0; i < b->vobu_count; i++) {
            const mvob_vobu_entry_t *e = &b->vobu_index[i];
            bool in_cell = (cell->last_sector > 0)
                           ? (e->orig_lbn >= cell->first_sector && e->orig_lbn <= cell->last_sector)
                           : (e->vob_id == pos->vob_id_nr && e->c_id == pos->cell_nr);
            if (in_cell) cell_vobu_count++;
        }
        bool is_static_still_cell = (cell->still_time > 0 && cell_vobu_count == 1);

        /* Play all VOBUs belonging to this Cell */
        bool interrupted = false;
        for (size_t i = 0; i < b->vobu_count && b->running; i++) {
            if (b->seek_target_vobu_idx >= 0) {
                if (i < (size_t)b->seek_target_vobu_idx) {
                    continue;
                }
                b->seek_target_vobu_idx = -1;
            }
            const mvob_vobu_entry_t *e = &b->vobu_index[i];
            bool in_cell = false;
            if (cell->last_sector > 0) {
                in_cell = (e->orig_lbn >= cell->first_sector && e->orig_lbn <= cell->last_sector);
            } else {
                in_cell = (e->vob_id == pos->vob_id_nr && e->c_id == pos->cell_nr);
            }
            if (!in_cell) continue;

            b->current_vobu_idx = i;
            if (play_vobu_entry(b, e, is_static_still_cell)) {
                interrupted = true;
                break;
            }
        }

        if (interrupted || !b->running) {
            continue;
        }

        /* Handle Cell still_time (0xFF = infinite menu/still wait; 1..254 = N seconds) */
        if (cell->still_time > 0) {
            b->user_paused = true;
            if (b->resolving_root_menu && (b->domain == DVD_DOMAIN_VTSM || b->domain == DVD_DOMAIN_VMGM)) {
                b->effective_root_vts = b->vts_nr;
                b->effective_root_pgc_nr = b->pgc_nr;
            }
            if (b->player) {
                player_adapter_set_paused(b->player, true);
            }
            if (b->event_cb) {
                dvd_vm_event_t ev_pause;
                memset(&ev_pause, 0, sizeof(ev_pause));
                ev_pause.type = DVD_VM_EVENT_PAUSE;
                ev_pause.paused = true;
                b->event_cb(b->event_user_data, &ev_pause);
            }
            b->expected_next_pts_nanos = -1;
            uint64_t still_start = monotonic_nanos();
            uint64_t still_dur = (cell->still_time == 0xFF)
                                 ? UINT64_MAX
                                 : ((uint64_t)cell->still_time * 1000000000ULL);
            while (b->running) {
                if (process_pending_user_action(b)) {
                    interrupted = true;
                    break;
                }
                if (getenv("TINYDVD_FAST_TEST") != NULL) {
                    /* In fast-forward test mode, stop waiting if interactive menu buttons are active */
                    if (b->has_active_hli && b->active_hli.hl_gi.btn_ns > 0) {
                        break;
                    }
                }
                if (cell->still_time != 0xFF && (monotonic_nanos() - still_start) >= still_dur) {
                    break;
                }
                usleep(15000);
            }
            if (cell->still_time != 0xFF) {
                b->user_paused = false;
            }
            if (interrupted || !b->running) {
                continue;
            }
        }

        /* Execute Cell Command if present */
        if (cell->cell_cmd_nr > 0 && pgc->command_tbl &&
            cell->cell_cmd_nr <= pgc->command_tbl->nr_of_cell) {
            const vm_cmd_t *ccmd = &pgc->command_tbl->cell_cmds[cell->cell_cmd_nr - 1];
            vm_link_result_t res = vm_eval_cmd(b, ccmd, NULL, NULL);
            if (res == VM_LINK_PGC_CHANGED || res == VM_LINK_EXIT) {
                continue;
            } else if (res == VM_LINK_TAIL_PGC) {
                b->cell_nr = pgc->nr_of_cells + 1;
                continue;
            } else if (res == VM_LINK_TOP_CELL) {
                continue;
            } else if (res == VM_LINK_TOP_PGC) {
                vm_enter_pgc(b, b->domain, b->vts_nr, b->pgc_nr, 1);
                continue;
            }
        }

        /* Advance to next Cell in PGC */
        b->cell_nr = cur_cell_nr + 1;
    }

    b->running = false;
    return NULL;
}

/* =========================================================================
 * Public Lifecycle & User Input API
 * ========================================================================= */

static void dvdread_silent_logger(void *priv, dvd_logger_level_t level, const char *fmt, va_list list) {
    (void)priv;
    (void)level;
    (void)fmt;
    (void)list;
}

dvd_vm_bridge_t *dvd_vm_bridge_init(const char *dvd_path, player_adapter_t *player, spu_compositor_t *spu) {
    if (!dvd_path) return NULL;

    dvd_vm_bridge_t *b = (dvd_vm_bridge_t *)calloc(1, sizeof(dvd_vm_bridge_t));
    if (!b) return NULL;

    b->dvd_path = strdup(dvd_path);
    b->player = player;
    b->spu = spu;
    b->expected_next_pts_nanos = -1;
    b->seek_target_vobu_idx = -1;
    pthread_mutex_init(&b->lock, NULL);

    /* Initialize standard DVD System Parameter Registers (SPRMs) */
    b->sprm[0]  = 0x656e; /* 'en' menu language */
    b->sprm[1]  = 0;      /* Audio stream 0 */
    b->sprm[2]  = 0;      /* Subpicture stream 0 (off) */
    b->sprm[3]  = 1;      /* Angle 1 */
    b->sprm[4]  = 1;      /* Title Track Number 1 */
    b->sprm[5]  = 1;      /* VTS Title Track Number 1 */
    b->sprm[6]  = 1;      /* PGC Number 1 */
    b->sprm[7]  = 1;      /* Part of Title (Chapter) 1 */
    b->sprm[8]  = 0x0400; /* Highlighted Button 1 (1 << 10) */
    b->sprm[13] = 15;     /* Parental level 15 */
    b->sprm[14] = 0x0100; /* 16:9 Widescreen display */
    b->sprm[16] = 0x656e; /* Initial audio language 'en' */
    b->sprm[18] = 0x656e; /* Initial subpicture language 'en' */
    b->sprm[20] = 0x01;   /* Player region code 1 */

    const char *ifo_open_path = dvd_path;
    if (tdvd_is_archive_path(dvd_path)) {
        b->archive = tdvd_archive_open(dvd_path);
        if (!b->archive || !tdvd_archive_ifo_dir(b->archive)) {
            fprintf(stderr, "[dvd_vm] Failed to open .tdvd archive at %s\n", dvd_path);
            if (b->archive) tdvd_archive_close(b->archive);
            pthread_mutex_destroy(&b->lock);
            free(b->dvd_path);
            free(b);
            return NULL;
        }
        ifo_open_path = tdvd_archive_ifo_dir(b->archive);
    }

    dvd_logger_cb logcb = { .pf_log = dvdread_silent_logger };
    b->reader = DVDOpen2(NULL, &logcb, ifo_open_path);
    if (!b->reader) {
        fprintf(stderr, "[dvd_vm] Failed to open DVD at %s\n", dvd_path);
        if (b->archive) tdvd_archive_close(b->archive);
        pthread_mutex_destroy(&b->lock);
        free(b->dvd_path);
        free(b);
        return NULL;
    }

    b->vmgi = ifoOpen(b->reader, 0);
    if (!b->vmgi) {
        fprintf(stderr, "[dvd_vm] Failed to open VIDEO_TS.IFO in %s\n", dvd_path);
        DVDClose(b->reader);
        if (b->archive) tdvd_archive_close(b->archive);
        pthread_mutex_destroy(&b->lock);
        free(b->dvd_path);
        free(b);
        return NULL;
    }

    return b;
}

bool dvd_vm_bridge_start(dvd_vm_bridge_t *b) {
    if (!b || b->running) return false;
    b->running = true;
    if (pthread_create(&b->thread, NULL, vm_thread_func, b) != 0) {
        b->running = false;
        return false;
    }
    b->thread_started = true;
    return true;
}

void dvd_vm_bridge_stop(dvd_vm_bridge_t *b) {
    if (!b) return;
    b->running = false;
    if (b->thread_started) {
        pthread_join(b->thread, NULL);
        b->thread_started = false;
    }
}

void dvd_vm_bridge_destroy(dvd_vm_bridge_t *b) {
    if (!b) return;
    dvd_vm_bridge_stop(b);
    close_domain_files(b);
    if (b->vobu_index) free(b->vobu_index);
    for (int i = 1; i < 100; i++) {
        if (b->vtsi[i]) ifoClose(b->vtsi[i]);
    }
    if (b->vmgi) ifoClose(b->vmgi);
    if (b->reader) DVDClose(b->reader);
    if (b->archive) tdvd_archive_close(b->archive);
    pthread_mutex_destroy(&b->lock);
    if (b->dvd_path) free(b->dvd_path);
    free(b);
}

static void queue_action(dvd_vm_bridge_t *b, user_action_type_t act, int x, int y) {
    if (!b) return;
    pthread_mutex_lock(&b->lock);
    /* Do not let mouse motion overwrite an unprocessed key press or mouse click */
    if (act == ACT_MOUSE_MOVE && b->pending_action != ACT_NONE && b->pending_action != ACT_MOUSE_MOVE) {
        pthread_mutex_unlock(&b->lock);
        return;
    }
    b->pending_action = act;
    b->action_x = x;
    b->action_y = y;
    pthread_mutex_unlock(&b->lock);
}

void dvd_vm_bridge_mouse_move(dvd_vm_bridge_t *b, int x, int y)  { queue_action(b, ACT_MOUSE_MOVE, x, y); }
void dvd_vm_bridge_mouse_click(dvd_vm_bridge_t *b, int x, int y) { queue_action(b, ACT_MOUSE_CLICK, x, y); }
void dvd_vm_bridge_up_button(dvd_vm_bridge_t *b)                 { queue_action(b, ACT_UP, 0, 0); }
void dvd_vm_bridge_down_button(dvd_vm_bridge_t *b)               { queue_action(b, ACT_DOWN, 0, 0); }
void dvd_vm_bridge_left_button(dvd_vm_bridge_t *b)               { queue_action(b, ACT_LEFT, 0, 0); }
void dvd_vm_bridge_right_button(dvd_vm_bridge_t *b)              { queue_action(b, ACT_RIGHT, 0, 0); }
void dvd_vm_bridge_activate_button(dvd_vm_bridge_t *b)           { queue_action(b, ACT_ACTIVATE, 0, 0); }
void dvd_vm_bridge_menu(dvd_vm_bridge_t *b)                      { queue_action(b, ACT_MENU, 0, 0); }

bool dvd_vm_bridge_escape(dvd_vm_bridge_t *b) {
    if (!b || !b->running) return false;

    pthread_mutex_lock(&b->lock);

    /* If playing a movie or title clip, Esc takes you up to the Root Menu if available */
    if (b->domain == DVD_DOMAIN_VTSTT) {
        int target_vts = (b->vts_nr >= 1) ? b->vts_nr : 1;
        int pgcn = (b->effective_root_pgc_nr > 0 && b->effective_root_vts == target_vts)
                   ? b->effective_root_pgc_nr
                   : find_menu_pgc_by_entry_id(b, DVD_DOMAIN_VTSM, target_vts, 3);
        pgc_t *menu_pgc = lookup_pgc(b, DVD_DOMAIN_VTSM, target_vts, pgcn);
        if (target_vts != 1 && is_dummy_rsm_menu(menu_pgc)) {
            int vts1_pgcn = find_menu_pgc_by_entry_id(b, DVD_DOMAIN_VTSM, 1, 3);
            pgc_t *vts1_menu = lookup_pgc(b, DVD_DOMAIN_VTSM, 1, vts1_pgcn);
            if (vts1_menu && !is_dummy_rsm_menu(vts1_menu)) {
                menu_pgc = vts1_menu;
            }
        }
        pgc_t *vmgm_menu = lookup_pgc(b, DVD_DOMAIN_VMGM, 0, 1);
        bool has_menu = (!is_dummy_rsm_menu(menu_pgc) || vmgm_menu != NULL);
        if (!has_menu) {
            pthread_mutex_unlock(&b->lock);
            return false; /* No menu on disc to go up to: Esc exits */
        }
        b->pending_action = ACT_ESCAPE;
        pthread_mutex_unlock(&b->lock);
        return true;
    }

    /* If inside a Video Title Set Menu, check if we can go up a menu level */
    if (b->domain == DVD_DOMAIN_VTSM) {
        int target_vts = (b->vts_nr >= 1) ? b->vts_nr : 1;
        int root_pgcn = (b->effective_root_pgc_nr > 0 && b->effective_root_vts == target_vts)
                        ? b->effective_root_pgc_nr
                        : find_menu_pgc_by_entry_id(b, DVD_DOMAIN_VTSM, target_vts, 3);

        /* Case A: Menu PGC defines an explicit parent GoUp menu */
        if (b->current_pgc && b->current_pgc->goup_pgc_nr > 0 &&
            b->current_pgc->goup_pgc_nr != b->pgc_nr) {
            b->pending_action = ACT_ESCAPE;
            pthread_mutex_unlock(&b->lock);
            return true;
        }

        /* Case B: Inside a sub-menu (Scene selection, audio, languages, etc.) and not root */
        if (b->pgc_nr != root_pgcn) {
            b->pending_action = ACT_ESCAPE;
            pthread_mutex_unlock(&b->lock);
            return true;
        }
    }

    /* Already at the top-level Root Menu or Title Menu: cannot navigate up any further */
    pthread_mutex_unlock(&b->lock);
    return false;
}

void dvd_vm_bridge_toggle_pause(dvd_vm_bridge_t *b)              { queue_action(b, ACT_TOGGLE_PAUSE, 0, 0); }
void dvd_vm_bridge_seek_relative(dvd_vm_bridge_t *b, int delta_seconds) {
    queue_action(b, ACT_SEEK_REL, delta_seconds, 0);
}
void dvd_vm_bridge_seek_fraction(dvd_vm_bridge_t *b, double fraction_0_to_1) {
    if (!b) return;
    pthread_mutex_lock(&b->lock);
    b->pending_action = ACT_SEEK_FRAC;
    b->action_frac = fraction_0_to_1;
    pthread_mutex_unlock(&b->lock);
}
void dvd_vm_bridge_next_chapter(dvd_vm_bridge_t *b)              { queue_action(b, ACT_NEXT_CHAPTER, 0, 0); }
void dvd_vm_bridge_prev_chapter(dvd_vm_bridge_t *b)              { queue_action(b, ACT_PREV_CHAPTER, 0, 0); }
void dvd_vm_bridge_cycle_subtitles(dvd_vm_bridge_t *b)           { queue_action(b, ACT_CYCLE_SUBS, 0, 0); }
void dvd_vm_bridge_cycle_audio(dvd_vm_bridge_t *b)               { queue_action(b, ACT_CYCLE_AUDIO, 0, 0); }

bool dvd_vm_bridge_is_running(dvd_vm_bridge_t *b) {
    return b ? b->running : false;
}

void dvd_vm_bridge_set_event_callback(dvd_vm_bridge_t *b, dvd_vm_event_callback_t cb, void *user_data) {
    if (!b) return;
    pthread_mutex_lock(&b->lock);
    b->event_cb = cb;
    b->event_user_data = user_data;
    pthread_mutex_unlock(&b->lock);
}

spu_compositor_t *dvd_vm_bridge_get_spu(dvd_vm_bridge_t *b) {
    return b ? b->spu : NULL;
}

const char *dvd_vm_bridge_get_loaded_video(dvd_vm_bridge_t *b) {
    return b ? b->current_video_name : "";
}

int64_t dvd_vm_bridge_get_current_pts_nanos(dvd_vm_bridge_t *b) {
    return b ? b->current_pts_nanos : 0;
}

bool dvd_vm_bridge_is_paused(dvd_vm_bridge_t *b) {
    return b ? b->user_paused : false;
}

void dvd_vm_bridge_set_paused(dvd_vm_bridge_t *b, bool paused) {
    if (!b) return;
    pthread_mutex_lock(&b->lock);
    b->user_paused = paused;
    if (b->player) {
        player_adapter_set_paused(b->player, paused);
    }
    pthread_mutex_unlock(&b->lock);
}
