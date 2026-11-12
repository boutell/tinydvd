#include "player_adapter.h"
#include <string.h>

#define OP(a, name) ((a)->ops && (a)->ops->name)

static void reset_locked(player_adapter_t *a) {
    a->cmd_seq = 0;
    a->load_seq = 0;
    a->seek_seq = 0;
    a->file_loaded = false;
    a->seek_in_flight = false;
    a->seek_pending = false;
    a->pending_seek_nanos = 0;
    a->pending_seek_paused = false;
    a->paused = false;
    a->ended = false;
    a->pts_nanos = 0;
}

void player_adapter_init(player_adapter_t *a, const player_adapter_ops_t *ops, void *ctx) {
    if (!a) return;
    a->ops = ops;
    a->ctx = ctx;
    pthread_mutex_init(&a->lock, NULL);
    reset_locked(a);
}

void player_adapter_destroy(player_adapter_t *a) {
    if (!a) return;
    pthread_mutex_destroy(&a->lock);
}

void player_adapter_reset(player_adapter_t *a) {
    if (!a) return;
    pthread_mutex_lock(&a->lock);
    reset_locked(a);
    pthread_mutex_unlock(&a->lock);
}

/* A report tagged with seq is stale if a newer command of the relevant kind exists */
static bool is_stale(uint64_t seq, uint64_t newest) {
    return seq != 0 && seq < newest;
}

/* =========================================================================
 * Commands
 * ========================================================================= */

int player_adapter_load_file(player_adapter_t *a, const char *video_path, const char *subfile_name, const char *audio_path) {
    if (!a) return -1;
    int rc = 0;
    pthread_mutex_lock(&a->lock);
    uint64_t seq = ++a->cmd_seq;
    a->load_seq = seq;
    a->file_loaded = false;
    a->seek_in_flight = false;
    a->seek_pending = false;
    a->ended = false;
    a->pts_nanos = 0;
    if (OP(a, load_file)) {
        rc = a->ops->load_file(a->ctx, video_path, subfile_name, audio_path, seq);
    }
    pthread_mutex_unlock(&a->lock);
    return rc;
}

void player_adapter_seek_exact(player_adapter_t *a, uint64_t pts_nanos, bool paused) {
    if (!a) return;
    pthread_mutex_lock(&a->lock);
    uint64_t seq = ++a->cmd_seq;
    a->seek_seq = seq;
    a->pts_nanos = pts_nanos;
    a->paused = paused;
    a->ended = false;
    if (a->file_loaded) {
        a->seek_pending = false;
        a->seek_in_flight = true;
        if (OP(a, seek_exact)) a->ops->seek_exact(a->ctx, pts_nanos, paused, seq);
    } else {
        /* File still loading: dispatch when the backend reports file_loaded */
        a->seek_pending = true;
        a->pending_seek_nanos = pts_nanos;
        a->pending_seek_paused = paused;
    }
    pthread_mutex_unlock(&a->lock);
}

void player_adapter_set_paused(player_adapter_t *a, bool paused) {
    if (!a) return;
    pthread_mutex_lock(&a->lock);
    a->paused = paused;
    if (a->seek_pending) a->pending_seek_paused = paused;
    if (OP(a, set_paused)) a->ops->set_paused(a->ctx, paused);
    pthread_mutex_unlock(&a->lock);
}

void player_adapter_set_audio_track(player_adapter_t *a, int aid) {
    if (!a) return;
    pthread_mutex_lock(&a->lock);
    if (OP(a, set_audio_track)) a->ops->set_audio_track(a->ctx, aid);
    pthread_mutex_unlock(&a->lock);
}

void player_adapter_set_subtitle_track(player_adapter_t *a, int sid) {
    if (!a) return;
    pthread_mutex_lock(&a->lock);
    if (OP(a, set_subtitle_track)) a->ops->set_subtitle_track(a->ctx, sid);
    pthread_mutex_unlock(&a->lock);
}

void player_adapter_osd_message(player_adapter_t *a, const char *msg) {
    if (!a) return;
    pthread_mutex_lock(&a->lock);
    if (OP(a, osd_message)) a->ops->osd_message(a->ctx, msg);
    pthread_mutex_unlock(&a->lock);
}

void player_adapter_on_highlight(player_adapter_t *a, const dvd_vm_highlight_info_t *hl) {
    if (!a) return;
    pthread_mutex_lock(&a->lock);
    if (OP(a, on_highlight)) a->ops->on_highlight(a->ctx, hl);
    pthread_mutex_unlock(&a->lock);
}

void player_adapter_on_state(player_adapter_t *a, int domain, int vts_nr, int pgc_nr, int cell_nr, int chapter_nr) {
    if (!a) return;
    pthread_mutex_lock(&a->lock);
    if (OP(a, on_state)) a->ops->on_state(a->ctx, domain, vts_nr, pgc_nr, cell_nr, chapter_nr);
    pthread_mutex_unlock(&a->lock);
}

/* =========================================================================
 * Queries
 * ========================================================================= */

#define LOCKED_GET(type, expr) \
    if (!a) return (type)0; \
    pthread_mutex_lock(&a->lock); \
    type v = (expr); \
    pthread_mutex_unlock(&a->lock); \
    return v;

uint64_t player_adapter_get_pts_nanos(player_adapter_t *a)  { LOCKED_GET(uint64_t, a->pts_nanos) }
bool     player_adapter_is_paused(player_adapter_t *a)      { LOCKED_GET(bool, a->paused) }
bool     player_adapter_is_ended(player_adapter_t *a)       { LOCKED_GET(bool, a->ended) }
bool     player_adapter_is_file_loaded(player_adapter_t *a) { LOCKED_GET(bool, a->file_loaded) }
bool     player_adapter_is_seek_in_flight(player_adapter_t *a) { LOCKED_GET(bool, a->seek_in_flight) }
bool     player_adapter_is_ready(player_adapter_t *a) {
    LOCKED_GET(bool, a->file_loaded && !a->seek_in_flight && !a->seek_pending)
}
uint64_t player_adapter_get_seq(player_adapter_t *a)        { LOCKED_GET(uint64_t, a->cmd_seq) }

/* =========================================================================
 * Backend notifications
 * ========================================================================= */

void player_adapter_notify_file_loaded(player_adapter_t *a, uint64_t seq) {
    if (!a) return;
    pthread_mutex_lock(&a->lock);
    if (!is_stale(seq, a->load_seq)) {
        a->file_loaded = true;
        if (a->seek_pending) {
            a->seek_pending = false;
            a->seek_in_flight = true;
            if (OP(a, seek_exact)) {
                a->ops->seek_exact(a->ctx, a->pending_seek_nanos, a->pending_seek_paused, a->seek_seq);
            }
        }
    }
    pthread_mutex_unlock(&a->lock);
}

void player_adapter_notify_seek_completed(player_adapter_t *a, uint64_t seq, int64_t pts_nanos) {
    if (!a) return;
    pthread_mutex_lock(&a->lock);
    if (!is_stale(seq, a->seek_seq) && a->file_loaded && !a->seek_pending) {
        a->seek_in_flight = false;
        if (pts_nanos >= 0) a->pts_nanos = (uint64_t)pts_nanos;
    }
    pthread_mutex_unlock(&a->lock);
}

/* Position / end-of-file reports only count once the current file is loaded and settled */
static bool settled_locked(player_adapter_t *a, uint64_t seq) {
    return !is_stale(seq, a->load_seq) && !is_stale(seq, a->seek_seq) &&
           a->file_loaded && !a->seek_in_flight && !a->seek_pending;
}

void player_adapter_notify_time_pos(player_adapter_t *a, uint64_t seq, int64_t pts_nanos) {
    if (!a || pts_nanos < 0) return;
    pthread_mutex_lock(&a->lock);
    if (settled_locked(a, seq)) a->pts_nanos = (uint64_t)pts_nanos;
    pthread_mutex_unlock(&a->lock);
}

void player_adapter_notify_paused(player_adapter_t *a, bool paused) {
    if (!a) return;
    pthread_mutex_lock(&a->lock);
    a->paused = paused;
    pthread_mutex_unlock(&a->lock);
}

void player_adapter_notify_ended(player_adapter_t *a, uint64_t seq) {
    if (!a) return;
    pthread_mutex_lock(&a->lock);
    if (settled_locked(a, seq)) a->ended = true;
    pthread_mutex_unlock(&a->lock);
}
