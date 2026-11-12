#ifndef PLAYER_ADAPTER_H
#define PLAYER_ADAPTER_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * player_adapter: the single playback state machine shared by every video
 * backend (native mpv window, web browser <video> element).
 *
 * The DVD VM issues commands (load / seek / pause) through the adapter; the
 * adapter tracks load + seek progress and forwards the commands to the backend
 * via player_adapter_ops_t. Backends report progress back through the
 * player_adapter_notify_* functions. Backends hold NO playback state of their
 * own beyond what they need to talk to their player.
 *
 * Sequence numbers: every load/seek command gets a monotonically increasing
 * seq which is passed to the backend. Backends that can echo it back (the web
 * client) pass it to the notify_* functions so stale reports are discarded.
 * Backends that cannot (mpv, whose event queue is strictly ordered) pass 0,
 * which is always accepted.
 *
 * Locking: ops are invoked with the adapter lock held, so commands reach the
 * backend in the order they were issued. Ops must therefore never call back
 * into the adapter, and no code may call into the adapter while holding a
 * lock that an op also takes.
 */

typedef struct dvd_vm_button_rect {
    int btn_id;
    int x1, y1, x2, y2;
} dvd_vm_button_rect_t;

typedef struct dvd_vm_highlight_info {
    char *highlight_data_url;    /* Base64 PNG data URL or heap string (may be NULL) */
    int frame_w;
    int frame_h;
    int active_button;           /* 1-based, or 0 if none */
    int button_count;
    dvd_vm_button_rect_t buttons[36];
} dvd_vm_highlight_info_t;

typedef struct player_adapter_ops {
    int  (*load_file)(void *ctx, const char *video_path, const char *subfile_name, const char *audio_path, uint64_t seq);
    void (*seek_exact)(void *ctx, uint64_t pts_nanos, bool paused, uint64_t seq);
    void (*set_paused)(void *ctx, bool paused);
    void (*set_audio_track)(void *ctx, int aid);
    void (*set_subtitle_track)(void *ctx, int sid);
    void (*osd_message)(void *ctx, const char *msg);
    void (*on_highlight)(void *ctx, const dvd_vm_highlight_info_t *hl);
    void (*on_state)(void *ctx, int domain, int vts_nr, int pgc_nr, int cell_nr, int chapter_nr);
} player_adapter_ops_t;

typedef struct player_adapter {
    const player_adapter_ops_t *ops;
    void *ctx;

    pthread_mutex_t lock;
    uint64_t cmd_seq;          /* last seq handed out */
    uint64_t load_seq;         /* seq of the most recent load_file */
    uint64_t seek_seq;         /* seq of the most recent seek_exact */
    bool     file_loaded;      /* backend has the current file open */
    bool     seek_in_flight;   /* seek sent to backend, not yet completed */
    bool     seek_pending;     /* seek requested before file finished loading */
    uint64_t pending_seek_nanos;
    bool     pending_seek_paused;
    bool     paused;           /* last pause state requested or reported */
    bool     ended;            /* backend reached end of the current file */
    uint64_t pts_nanos;        /* best-known playback position */
} player_adapter_t;

void player_adapter_init(player_adapter_t *adapter, const player_adapter_ops_t *ops, void *ctx);
void player_adapter_destroy(player_adapter_t *adapter);

/* Forget all playback state (e.g. when a new disc's VM takes over the adapter). */
void player_adapter_reset(player_adapter_t *adapter);

/* Commands (invoked by dvd_vm_bridge) */
int  player_adapter_load_file(player_adapter_t *adapter, const char *video_path, const char *subfile_name, const char *audio_path);
void player_adapter_seek_exact(player_adapter_t *adapter, uint64_t pts_nanos, bool paused);
void player_adapter_set_paused(player_adapter_t *adapter, bool paused);
void player_adapter_set_audio_track(player_adapter_t *adapter, int aid);
void player_adapter_set_subtitle_track(player_adapter_t *adapter, int sid);
void player_adapter_osd_message(player_adapter_t *adapter, const char *msg);
void player_adapter_on_highlight(player_adapter_t *adapter, const dvd_vm_highlight_info_t *hl);
void player_adapter_on_state(player_adapter_t *adapter, int domain, int vts_nr, int pgc_nr, int cell_nr, int chapter_nr);

/* State queries (used by the VM's VOBU clock) */
uint64_t player_adapter_get_pts_nanos(player_adapter_t *adapter);
bool     player_adapter_is_paused(player_adapter_t *adapter);
bool     player_adapter_is_ended(player_adapter_t *adapter);
bool     player_adapter_is_file_loaded(player_adapter_t *adapter);
bool     player_adapter_is_seek_in_flight(player_adapter_t *adapter);
/* File loaded and no seek pending or in flight */
bool     player_adapter_is_ready(player_adapter_t *adapter);
uint64_t player_adapter_get_seq(player_adapter_t *adapter);

/* Backend notifications (seq 0 = backend cannot track seq; always accepted) */
void player_adapter_notify_file_loaded(player_adapter_t *adapter, uint64_t seq);
void player_adapter_notify_seek_completed(player_adapter_t *adapter, uint64_t seq, int64_t pts_nanos);
void player_adapter_notify_time_pos(player_adapter_t *adapter, uint64_t seq, int64_t pts_nanos);
void player_adapter_notify_paused(player_adapter_t *adapter, bool paused);
void player_adapter_notify_ended(player_adapter_t *adapter, uint64_t seq);

#ifdef __cplusplus
}
#endif

#endif /* PLAYER_ADAPTER_H */
