#include "web_server.h"
#include "web_assets.h"
#include "library.h"
#include "dvd_vm_bridge.h"
#include "spu_compositor.h"
#include "tdvd_archive.h"
#include "spu_png.h"
#include "sha1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <ctype.h>

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define CHUNK_SIZE 65536
#define SESSION_TIMEOUT_SEC 180

typedef struct ws_client {
    int fd;
    bool active;
    struct ws_client *next;
} ws_client_t;

typedef struct web_session web_session_t;

struct web_session {
    char id[64];
    char disc_path[1024];
    char disc_id[128];
    char disc_title[256];

    dvd_vm_bridge_t *vm;
    player_adapter_t adapter;   /* web backend of the shared player state machine */
    spu_compositor_t *spu;
    pthread_mutex_t lock;       /* guards vm/spu lifetime and disc identity */

    ws_client_t *ws_clients;
    pthread_mutex_t ws_lock;
    int client_count;

    /* Snapshot of what clients should be showing, replayed to (re)connecting
     * clients. Guarded by state_lock, which adapter ops take; adapter ops must
     * never take `lock` (the ws thread holds it while notifying the adapter). */
    pthread_mutex_t state_lock;
    char last_video[128];
    int64_t last_pts_nanos;
    bool last_paused;
    char *last_highlight_url;
    int last_frame_w;
    int last_frame_h;
    int last_active_btn;
    int last_button_count;
    dvd_vm_button_rect_t last_buttons[36];

    time_t last_active_time;
    web_session_t *next;
    web_server_t *server;
};

struct web_server {
    int listen_fd;
    int port;
    char bind_addr[64];

    tdvd_library_t lib;

    web_session_t *sessions;
    pthread_mutex_t sessions_lock;

    volatile bool running;
    pthread_t accept_thread;
};

static void json_escape_copy(const char *in, char *out, size_t out_len);

/* =========================================================================
 * WebSocket Helpers (RFC 6455)
 * ========================================================================= */

static bool ws_send_text(int client_fd, const char *text, size_t len) {
    uint8_t header[10];
    size_t hlen = 0;
    header[0] = 0x81; /* FIN + Text Frame */

    if (len <= 125) {
        header[1] = (uint8_t)len;
        hlen = 2;
    } else if (len <= 65535) {
        header[1] = 126;
        header[2] = (uint8_t)((len >> 8) & 0xFF);
        header[3] = (uint8_t)(len & 0xFF);
        hlen = 4;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; i++) {
            header[2 + i] = (uint8_t)((len >> ((7 - i) * 8)) & 0xFF);
        }
        hlen = 10;
    }

    if (send(client_fd, header, hlen, MSG_NOSIGNAL) < 0) return false;
    if (send(client_fd, text, len, MSG_NOSIGNAL) < 0) return false;
    return true;
}

static void session_ws_broadcast(web_session_t *sess, const char *json) {
    if (!sess || !json) return;
    size_t len = strlen(json);

    pthread_mutex_lock(&sess->ws_lock);
    ws_client_t *c = sess->ws_clients;
    while (c) {
        if (c->active) {
            if (!ws_send_text(c->fd, json, len)) {
                c->active = false;
            }
        }
        c = c->next;
    }
    pthread_mutex_unlock(&sess->ws_lock);
}

/* Forward declarations for web player adapter */
static int web_adapter_load_file(void *ctx, const char *video_path, const char *subfile_name, const char *audio_path, uint64_t seq);
static void web_adapter_seek_exact(void *ctx, uint64_t pts_nanos, bool paused, uint64_t seq);
static void web_adapter_set_paused(void *ctx, bool paused);
static void web_adapter_set_audio_track(void *ctx, int aid);
static void web_adapter_set_subtitle_track(void *ctx, int sid);
static void web_adapter_osd_message(void *ctx, const char *msg);
static void web_adapter_on_highlight(void *ctx, const dvd_vm_highlight_info_t *hl);
static void web_adapter_on_state(void *ctx, int domain, int vts_nr, int pgc_nr, int cell_nr, int chapter_nr);

static const player_adapter_ops_t s_web_player_ops = {
    .load_file = web_adapter_load_file,
    .seek_exact = web_adapter_seek_exact,
    .set_paused = web_adapter_set_paused,
    .set_audio_track = web_adapter_set_audio_track,
    .set_subtitle_track = web_adapter_set_subtitle_track,
    .osd_message = web_adapter_osd_message,
    .on_highlight = web_adapter_on_highlight,
    .on_state = web_adapter_on_state,
};

static void session_ws_broadcast(web_session_t *sess, const char *text);

static int web_adapter_load_file(void *ctx, const char *video_path, const char *subfile_name, const char *audio_path, uint64_t seq) {
    (void)audio_path;
    web_session_t *sess = (web_session_t *)ctx;
    if (!sess) return -1;

    char base[64] = {0};
    const char *target_name = (subfile_name && subfile_name[0]) ? subfile_name : video_path;
    if (target_name) {
        const char *slash = strrchr(target_name, '/');
        const char *fn = slash ? slash + 1 : target_name;
        strncpy(base, fn, sizeof(base) - 1);
        char *dot = strrchr(base, '.');
        if (dot) *dot = '\0';
    }

    pthread_mutex_lock(&sess->state_lock);
    strncpy(sess->last_video, base, sizeof(sess->last_video) - 1);
    sess->last_pts_nanos = 0;
    sess->last_paused = false;
    if (sess->last_highlight_url) {
        free(sess->last_highlight_url);
        sess->last_highlight_url = NULL;
    }
    sess->last_button_count = 0;
    sess->last_active_btn = 0;
    pthread_mutex_unlock(&sess->state_lock);

    char buf[1024];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"video_load\",\"src\":\"/api/media/%s.mp4?disc=%s\",\"pts\":0.000,\"paused\":false,\"seq\":%llu}",
             base, sess->disc_id, (unsigned long long)seq);
    session_ws_broadcast(sess, buf);
    session_ws_broadcast(sess, "{\"type\":\"highlight\",\"data_url\":null,\"w\":720,\"h\":480,\"active_btn\":0,\"buttons\":[]}");
    return 0;
}

static void web_adapter_seek_exact(void *ctx, uint64_t pts_nanos, bool paused, uint64_t seq) {
    web_session_t *sess = (web_session_t *)ctx;
    if (!sess) return;

    pthread_mutex_lock(&sess->state_lock);
    sess->last_pts_nanos = (int64_t)pts_nanos;
    sess->last_paused = paused;
    pthread_mutex_unlock(&sess->state_lock);

    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"seek\",\"pts\":%.3f,\"paused\":%s,\"seq\":%llu}",
             (double)pts_nanos / 1e9, paused ? "true" : "false", (unsigned long long)seq);
    session_ws_broadcast(sess, buf);
}

static void web_adapter_set_paused(void *ctx, bool paused) {
    web_session_t *sess = (web_session_t *)ctx;
    if (!sess) return;

    pthread_mutex_lock(&sess->state_lock);
    sess->last_paused = paused;
    pthread_mutex_unlock(&sess->state_lock);

    char buf[256];
    snprintf(buf, sizeof(buf), "{\"type\":\"pause\",\"paused\":%s}", paused ? "true" : "false");
    session_ws_broadcast(sess, buf);
}

static void web_adapter_set_audio_track(void *ctx, int aid) {
    (void)ctx;
    (void)aid;
}

static void web_adapter_set_subtitle_track(void *ctx, int sid) {
    (void)ctx;
    (void)sid;
}

static void web_adapter_osd_message(void *ctx, const char *msg) {
    web_session_t *sess = (web_session_t *)ctx;
    if (!sess || !msg) return;
    char buf[512];
    snprintf(buf, sizeof(buf), "{\"type\":\"osd\",\"text\":\"%s\"}", msg);
    session_ws_broadcast(sess, buf);
}

static void web_adapter_on_highlight(void *ctx, const dvd_vm_highlight_info_t *hl) {
    web_session_t *sess = (web_session_t *)ctx;
    if (!sess || !hl) return;

    pthread_mutex_lock(&sess->state_lock);
    /* The VM refreshes highlights every VOBU; only tell clients about changes */
    bool same_url = (!hl->highlight_data_url && !sess->last_highlight_url) ||
                    (hl->highlight_data_url && sess->last_highlight_url &&
                     strcmp(hl->highlight_data_url, sess->last_highlight_url) == 0);
    bool unchanged = same_url &&
                     sess->last_frame_w == hl->frame_w && sess->last_frame_h == hl->frame_h &&
                     sess->last_active_btn == hl->active_button &&
                     sess->last_button_count == hl->button_count &&
                     memcmp(sess->last_buttons, hl->buttons,
                            sizeof(hl->buttons[0]) * (size_t)(hl->button_count > 36 ? 36 : hl->button_count)) == 0;
    pthread_mutex_unlock(&sess->state_lock);
    if (unchanged) return;

    char *hl_url_copy = hl->highlight_data_url ? strdup(hl->highlight_data_url) : NULL;

    pthread_mutex_lock(&sess->state_lock);
    if (sess->last_highlight_url) {
        free(sess->last_highlight_url);
        sess->last_highlight_url = NULL;
    }
    sess->last_highlight_url = hl_url_copy;
    sess->last_frame_w = hl->frame_w;
    sess->last_frame_h = hl->frame_h;
    sess->last_active_btn = hl->active_button;
    sess->last_button_count = hl->button_count;
    for (int i = 0; i < hl->button_count && i < 36; i++) {
        sess->last_buttons[i] = hl->buttons[i];
    }
    pthread_mutex_unlock(&sess->state_lock);

    char btn_json[4096] = "[";
    size_t bpos = 1;
    for (int i = 0; i < hl->button_count && i < 36; i++) {
        char item[128];
        snprintf(item, sizeof(item), "%s{\"btn\":%d,\"x1\":%d,\"y1\":%d,\"x2\":%d,\"y2\":%d}",
                 (i > 0) ? "," : "",
                 hl->buttons[i].btn_id,
                 hl->buttons[i].x1, hl->buttons[i].y1,
                 hl->buttons[i].x2, hl->buttons[i].y2);
        size_t ilen = strlen(item);
        if (bpos + ilen + 2 < sizeof(btn_json)) {
            strcpy(btn_json + bpos, item);
            bpos += ilen;
        }
    }
    strcpy(btn_json + bpos, "]");

    if (hl->highlight_data_url) {
        size_t dlen = strlen(hl->highlight_data_url);
        size_t msg_len = dlen + strlen(btn_json) + 256;
        char *dyn = (char *)malloc(msg_len);
        if (dyn) {
            snprintf(dyn, msg_len,
                     "{\"type\":\"highlight\",\"data_url\":\"%s\",\"w\":%d,\"h\":%d,\"active_btn\":%d,\"buttons\":%s}",
                     hl->highlight_data_url, hl->frame_w, hl->frame_h, hl->active_button, btn_json);
            session_ws_broadcast(sess, dyn);
            free(dyn);
        }
    } else {
        size_t msg_len = strlen(btn_json) + 256;
        char *dyn = (char *)malloc(msg_len);
        if (dyn) {
            snprintf(dyn, msg_len,
                     "{\"type\":\"highlight\",\"data_url\":null,\"w\":%d,\"h\":%d,\"active_btn\":%d,\"buttons\":%s}",
                     hl->frame_w, hl->frame_h, hl->active_button, btn_json);
            session_ws_broadcast(sess, dyn);
            free(dyn);
        }
    }
}

static void web_adapter_on_state(void *ctx, int domain, int vts_nr, int pgc_nr, int cell_nr, int chapter_nr) {
    web_session_t *sess = (web_session_t *)ctx;
    if (!sess) return;
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"domain_state\",\"domain\":%d,\"vts\":%d,\"pgc\":%d,\"cell\":%d,\"chapter\":%d}",
             domain, vts_nr, pgc_nr, cell_nr, chapter_nr);
    session_ws_broadcast(sess, buf);
}

/* =========================================================================
 * Session Lifecycle & Management
 * ========================================================================= */

static void session_destroy(web_session_t *sess) {
    if (!sess) return;

    dvd_vm_bridge_t *vm = NULL;
    spu_compositor_t *spu = NULL;

    pthread_mutex_lock(&sess->lock);
    vm = sess->vm;
    sess->vm = NULL;
    spu = sess->spu;
    sess->spu = NULL;
    pthread_mutex_unlock(&sess->lock);

    if (vm) {
        dvd_vm_bridge_destroy(vm);
    }
    if (spu) {
        spu_compositor_destroy(spu);
    }

    pthread_mutex_lock(&sess->ws_lock);
    ws_client_t *c = sess->ws_clients;
    while (c) {
        ws_client_t *next = c->next;
        close(c->fd);
        free(c);
        c = next;
    }
    sess->ws_clients = NULL;
    pthread_mutex_unlock(&sess->ws_lock);

    if (sess->last_highlight_url) free(sess->last_highlight_url);
    player_adapter_destroy(&sess->adapter);
    pthread_mutex_destroy(&sess->state_lock);
    pthread_mutex_destroy(&sess->lock);
    pthread_mutex_destroy(&sess->ws_lock);
    free(sess);
}

static web_session_t *get_or_create_session(web_server_t *s, const char *session_id, const char *disc_path, const char *disc_title) {
    if (!s) return NULL;

    char sid[64] = {0};
    if (session_id && session_id[0]) {
        tdvd_library_sanitize_id(session_id, sid, sizeof(sid));
    }
    if (sid[0] == '\0') {
        snprintf(sid, sizeof(sid), "s_%08x%08x", (unsigned)rand(), (unsigned)time(NULL));
    }

    pthread_mutex_lock(&s->sessions_lock);

    web_session_t *sess = s->sessions;
    while (sess) {
        if (strcmp(sess->id, sid) == 0) {
            sess->last_active_time = time(NULL);
            pthread_mutex_unlock(&s->sessions_lock);
            return sess;
        }
        sess = sess->next;
    }

    /* Not found: allocate new independent session */
    sess = (web_session_t *)calloc(1, sizeof(web_session_t));
    strncpy(sess->id, sid, sizeof(sess->id) - 1);
    sess->server = s;
    sess->last_active_time = time(NULL);
    pthread_mutex_init(&sess->lock, NULL);
    pthread_mutex_init(&sess->ws_lock, NULL);
    pthread_mutex_init(&sess->state_lock, NULL);
    player_adapter_init(&sess->adapter, &s_web_player_ops, sess);

    /* Single-disc mode auto-starts its only disc. In library mode a new session
     * (e.g. a fresh tab connecting its WebSocket) stays idle on the library
     * until the user picks a disc via /api/play. */
    pthread_mutex_lock(&s->lib.catalog_lock);
    bool is_lib = s->lib.is_library;
    char default_path_copy[1024];
    char default_title_copy[256];
    strncpy(default_path_copy, s->lib.default_disc_path, sizeof(default_path_copy) - 1);
    default_path_copy[sizeof(default_path_copy) - 1] = '\0';
    strncpy(default_title_copy, s->lib.default_disc_title, sizeof(default_title_copy) - 1);
    default_title_copy[sizeof(default_title_copy) - 1] = '\0';
    pthread_mutex_unlock(&s->lib.catalog_lock);

    const char *fallback_disc = is_lib ? NULL : default_path_copy;
    const char *target_disc = (disc_path && disc_path[0]) ? disc_path : fallback_disc;
    const char *target_title = (disc_title && disc_title[0]) ? disc_title : default_title_copy;

    if (target_disc && target_disc[0]) {
        strncpy(sess->disc_path, target_disc, sizeof(sess->disc_path) - 1);
        const char *slash = strrchr(target_disc, '/');
        const char *base = slash ? slash + 1 : target_disc;
        tdvd_library_sanitize_id(base, sess->disc_id, sizeof(sess->disc_id));
        strncpy(sess->disc_title, (target_title && target_title[0]) ? target_title : sess->disc_id, sizeof(sess->disc_title) - 1);

        sess->spu = spu_compositor_create();
        sess->vm = dvd_vm_bridge_init(sess->disc_path, &sess->adapter, sess->spu);
        if (sess->vm) {
            dvd_vm_bridge_start(sess->vm);
            fprintf(stderr, "[web_server] Started independent session %s for %s\n", sess->id, sess->disc_title);
        } else {
            fprintf(stderr, "[web_server] Failed to init VM for session %s\n", sess->id);
        }
    }

    sess->next = s->sessions;
    s->sessions = sess;

    pthread_mutex_unlock(&s->sessions_lock);
    return sess;
}

static bool session_load_movie(web_session_t *sess, const char *disc_path, const char *disc_title) {
    if (!sess || !disc_path) return false;

    dvd_vm_bridge_t *old_vm = NULL;
    spu_compositor_t *old_spu = NULL;

    pthread_mutex_lock(&sess->lock);
    old_vm = sess->vm;
    sess->vm = NULL;
    old_spu = sess->spu;
    sess->spu = NULL;
    pthread_mutex_unlock(&sess->lock);

    if (old_vm) dvd_vm_bridge_destroy(old_vm);
    if (old_spu) spu_compositor_destroy(old_spu);

    pthread_mutex_lock(&sess->lock);

    strncpy(sess->disc_path, disc_path, sizeof(sess->disc_path) - 1);
    const char *slash = strrchr(disc_path, '/');
    const char *base = slash ? slash + 1 : disc_path;
    tdvd_library_sanitize_id(base, sess->disc_id, sizeof(sess->disc_id));
    if (disc_title && disc_title[0]) {
        strncpy(sess->disc_title, disc_title, sizeof(sess->disc_title) - 1);
    } else {
        strncpy(sess->disc_title, sess->disc_id, sizeof(sess->disc_title) - 1);
    }

    pthread_mutex_lock(&sess->state_lock);
    sess->last_video[0] = '\0';
    sess->last_pts_nanos = 0;
    sess->last_paused = false;
    if (sess->last_highlight_url) {
        free(sess->last_highlight_url);
        sess->last_highlight_url = NULL;
    }
    sess->last_button_count = 0;
    sess->last_active_btn = 0;
    pthread_mutex_unlock(&sess->state_lock);

    /* Old VM is gone; the new one starts with a clean playback state */
    player_adapter_reset(&sess->adapter);
    sess->spu = spu_compositor_create();
    sess->vm = dvd_vm_bridge_init(sess->disc_path, &sess->adapter, sess->spu);
    if (!sess->vm) {
        if (sess->spu) {
            spu_compositor_destroy(sess->spu);
            sess->spu = NULL;
        }
        pthread_mutex_unlock(&sess->lock);
        return false;
    }

    dvd_vm_bridge_start(sess->vm);

    pthread_mutex_unlock(&sess->lock);

    char state_msg[1024];
    snprintf(state_msg, sizeof(state_msg), "{\"type\":\"state\",\"movie_title\":\"%s\",\"session_id\":\"%s\"}", sess->disc_title, sess->id);
    session_ws_broadcast(sess, state_msg);

    fprintf(stderr, "[web_server] Session %s switched to disc: %s (%s)\n", sess->id, sess->disc_title, disc_path);
    return true;
}

/* =========================================================================
 * Fast On-Demand Remuxing & Caching (Shared across all sessions)
 * ========================================================================= */

static bool ensure_remuxed_mp4(web_server_t *s, const char *disc_path, const char *disc_id, const char *base_name, char *out_path, size_t out_path_len) {
    if (!s || !base_name) return false;

    const char *dpath = (disc_path && disc_path[0]) ? disc_path : s->lib.default_disc_path;
    const char *did = (disc_id && disc_id[0]) ? disc_id : s->lib.default_disc_id;

    char cache_dir[512];
    snprintf(cache_dir, sizeof(cache_dir), "/tmp/tinydvd_web_cache/%s", did);
    mkdir("/tmp/tinydvd_web_cache", 0755);
    mkdir(cache_dir, 0755);

    snprintf(out_path, out_path_len, "%s/%s.mp4", cache_dir, base_name);

    struct stat st;
    if (stat(out_path, &st) == 0 && st.st_size > 0) {
        return true; /* Already cached */
    }

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", out_path);

    char mkv_name[128];
    snprintf(mkv_name, sizeof(mkv_name), "%s.mkv", base_name);

    fprintf(stderr, "[web_server] Remuxing %s -> %s...\n", mkv_name, out_path);

    if (tdvd_library_is_tdvd_file(dpath)) {
        tdvd_archive_t *arc = tdvd_archive_open(dpath);
        if (!arc) return false;

        uint64_t offset = 0, size = 0;
        if (!tdvd_archive_find(arc, mkv_name, &offset, &size)) {
            tdvd_archive_close(arc);
            return false;
        }

        FILE *fin = fopen(dpath, "rb");
        if (!fin) {
            tdvd_archive_close(arc);
            return false;
        }

        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "ffmpeg -y -hide_banner -loglevel error -i pipe:0 -c copy -movflags +faststart -f mp4 \"%s\"", tmp_path);
        FILE *p = popen(cmd, "w");
        if (!p) {
            fclose(fin);
            tdvd_archive_close(arc);
            return false;
        }

        fseeko(fin, offset, SEEK_SET);
        uint8_t buf[CHUNK_SIZE];
        uint64_t remaining = size;
        while (remaining > 0) {
            size_t to_read = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : (size_t)remaining;
            size_t n = fread(buf, 1, to_read, fin);
            if (n == 0) break;
            fwrite(buf, 1, n, p);
            remaining -= n;
        }

        pclose(p);
        fclose(fin);
        tdvd_archive_close(arc);
    } else {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "ffmpeg -y -hide_banner -loglevel error -i \"%s/%s\" -c copy -movflags +faststart -f mp4 \"%s\"",
                 dpath, mkv_name, tmp_path);
        int ret = system(cmd);
        (void)ret;
    }

    if (stat(tmp_path, &st) == 0 && st.st_size > 0) {
        rename(tmp_path, out_path);
        fprintf(stderr, "[web_server] Remux complete: %s (%llu bytes)\n", out_path, (unsigned long long)st.st_size);
        return true;
    }

    unlink(tmp_path);
    return false;
}

/* =========================================================================
 * HTTP Request Handler
 * ========================================================================= */

/* Remember the client's position so a reconnecting client resumes in place */
static void session_set_last_pts(web_session_t *sess, int64_t pts_nanos) {
    pthread_mutex_lock(&sess->state_lock);
    sess->last_pts_nanos = pts_nanos;
    pthread_mutex_unlock(&sess->state_lock);
}

static uint64_t json_get_u64(const char *payload, const char *key_with_colon) {
    const char *p = strstr(payload, key_with_colon);
    return p ? strtoull(p + strlen(key_with_colon), NULL, 10) : 0;
}

static bool json_get_pts_nanos(const char *payload, int64_t *out) {
    const char *p = strstr(payload, "\"pts\":");
    if (!p) return false;
    double pts = atof(p + 6);
    if (pts < 0) pts = 0;
    *out = (int64_t)(pts * 1e9);
    return true;
}

/* Browser <video> reports -> shared player adapter (the web backend's half of
 * what mpv_controller_poll_events does for the native player) */
static void handle_client_playback_report(web_session_t *sess, const char *payload) {
    player_adapter_t *a = &sess->adapter;
    uint64_t seq = json_get_u64(payload, "\"seq\":");
    int64_t pts_ns = -1;

    if (strstr(payload, "\"file_loaded\"")) {
        player_adapter_notify_file_loaded(a, seq);
    } else if (strstr(payload, "\"seek_completed\"")) {
        if (json_get_pts_nanos(payload, &pts_ns)) session_set_last_pts(sess, pts_ns);
        player_adapter_notify_seek_completed(a, seq, pts_ns);
    } else if (strstr(payload, "\"time_update\"")) {
        if (json_get_pts_nanos(payload, &pts_ns)) {
            session_set_last_pts(sess, pts_ns);
            player_adapter_notify_time_pos(a, seq, pts_ns);
        }
    } else if (strstr(payload, "\"client_playback\"")) {
        if (strstr(payload, "\"state\":\"paused\"")) {
            player_adapter_notify_paused(a, true);
        } else if (strstr(payload, "\"state\":\"playing\"")) {
            player_adapter_notify_paused(a, false);
        }
        if (json_get_pts_nanos(payload, &pts_ns)) {
            session_set_last_pts(sess, pts_ns);
            player_adapter_notify_time_pos(a, seq, pts_ns);
        }
    } else if (strstr(payload, "\"video_ended\"")) {
        player_adapter_notify_ended(a, seq);
    }
}

static void handle_client_websocket(web_server_t *s, int client_fd, const char *ws_key, const char *session_id) {
    /* Compute RFC 6455 Sec-WebSocket-Accept */
    char combined[256];
    snprintf(combined, sizeof(combined), "%s%s", ws_key, WS_GUID);
    uint8_t digest[20];
    sha1_compute((const uint8_t *)combined, strlen(combined), digest);
    char *accept_b64 = spu_base64_encode(digest, 20);
    if (!accept_b64) return;

    char resp[512];
    snprintf(resp, sizeof(resp),
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n\r\n",
             accept_b64);
    free(accept_b64);

    if (send(client_fd, resp, strlen(resp), MSG_NOSIGNAL) < 0) return;

    /* Get or create session for this client */
    web_session_t *sess = get_or_create_session(s, session_id, NULL, NULL);
    if (!sess) {
        close(client_fd);
        return;
    }

    /* Add client to session's ws_clients list */
    ws_client_t *wc = (ws_client_t *)calloc(1, sizeof(ws_client_t));
    wc->fd = client_fd;
    wc->active = true;

    pthread_mutex_lock(&sess->ws_lock);
    wc->next = sess->ws_clients;
    sess->ws_clients = wc;
    sess->client_count++;
    sess->last_active_time = time(NULL);
    pthread_mutex_unlock(&sess->ws_lock);

    pthread_mutex_lock(&sess->lock);
    if (sess->vm) dvd_vm_bridge_set_paused(sess->vm, false);

    /* Send initial session state to newly connected client. The client reports
     * file_loaded / seek_completed for the replayed video_load like for any
     * other, which is what lets the adapter consider the backend ready. */
    char init_msg[2048];
    snprintf(init_msg, sizeof(init_msg),
             "{\"type\":\"state\",\"movie_title\":\"%s\",\"session_id\":\"%s\"}",
             sess->disc_title, sess->id);
    ws_send_text(client_fd, init_msg, strlen(init_msg));
    pthread_mutex_unlock(&sess->lock);

    /* Read before taking state_lock: adapter ops take state_lock under the adapter lock */
    uint64_t seq = player_adapter_get_seq(&sess->adapter);
    pthread_mutex_lock(&sess->state_lock);

    if (sess->last_video[0]) {
        snprintf(init_msg, sizeof(init_msg),
                 "{\"type\":\"video_load\",\"src\":\"/api/media/%s.mp4?disc=%s\",\"pts\":%.3f,\"paused\":%s,\"seq\":%llu}",
                 sess->last_video, sess->disc_id, (double)sess->last_pts_nanos / 1e9, sess->last_paused ? "true" : "false", (unsigned long long)seq);
        ws_send_text(client_fd, init_msg, strlen(init_msg));
    }

    if (sess->last_highlight_url && sess->last_button_count > 0) {
        char btn_json[4096] = "[";
        size_t bpos = 1;
        for (int i = 0; i < sess->last_button_count && i < 36; i++) {
            char item[128];
            snprintf(item, sizeof(item), "%s{\"btn\":%d,\"x1\":%d,\"y1\":%d,\"x2\":%d,\"y2\":%d}",
                     (i > 0) ? "," : "",
                     sess->last_buttons[i].btn_id,
                     sess->last_buttons[i].x1, sess->last_buttons[i].y1,
                     sess->last_buttons[i].x2, sess->last_buttons[i].y2);
            size_t ilen = strlen(item);
            if (bpos + ilen + 2 < sizeof(btn_json)) {
                strcpy(btn_json + bpos, item);
                bpos += ilen;
            }
        }
        strcpy(btn_json + bpos, "]");

        size_t dlen = strlen(sess->last_highlight_url);
        size_t hmsg_len = dlen + strlen(btn_json) + 256;
        char *hdyn = (char *)malloc(hmsg_len);
        if (hdyn) {
            snprintf(hdyn, hmsg_len,
                     "{\"type\":\"highlight\",\"data_url\":\"%s\",\"w\":%d,\"h\":%d,\"active_btn\":%d,\"buttons\":%s}",
                     sess->last_highlight_url, sess->last_frame_w, sess->last_frame_h, sess->last_active_btn, btn_json);
            ws_send_text(client_fd, hdyn, strlen(hdyn));
            free(hdyn);
        }
    }
    pthread_mutex_unlock(&sess->state_lock);

    /* Read client frame loop */
    while (s->running && wc->active) {
        uint8_t hdr[2];
        ssize_t hrec = recv(client_fd, hdr, 2, MSG_WAITALL);
        if (hrec != 2) {
            break;
        }

        uint8_t opcode = hdr[0] & 0x0F;
        bool is_masked = (hdr[1] & 0x80) != 0;
        uint64_t payload_len = hdr[1] & 0x7F;

        if (opcode == 8) {
            break; /* Close */
        } else if (opcode == 9) {
            uint8_t pong[2] = { 0x8A, 0x00 };
            send(client_fd, pong, 2, MSG_NOSIGNAL);
            continue;
        }

        if (payload_len == 126) {
            uint8_t ext[2];
            if (recv(client_fd, ext, 2, MSG_WAITALL) != 2) break;
            payload_len = ((uint64_t)ext[0] << 8) | ext[1];
        } else if (payload_len == 127) {
            uint8_t ext[8];
            if (recv(client_fd, ext, 8, MSG_WAITALL) != 8) break;
            payload_len = 0;
            for (int i = 0; i < 8; i++) payload_len = (payload_len << 8) | ext[i];
        }

        uint8_t mask[4] = {0};
        if (is_masked) {
            if (recv(client_fd, mask, 4, MSG_WAITALL) != 4) break;
        }

        if (payload_len > 65536) break;

        char *payload = (char *)malloc(payload_len + 1);
        if (!payload) break;

        size_t total_recv = 0;
        while (total_recv < payload_len) {
            ssize_t nr = recv(client_fd, payload + total_recv, payload_len - total_recv, 0);
            if (nr <= 0) break;
            total_recv += nr;
        }
        if (total_recv < payload_len) {
            free(payload);
            break;
        }

        if (is_masked) {
            for (size_t i = 0; i < payload_len; i++) {
                payload[i] ^= mask[i % 4];
            }
        }
        payload[payload_len] = '\0';

        /* Dispatch user action to this session's DVD VM */
        pthread_mutex_lock(&sess->lock);
        sess->last_active_time = time(NULL);
        if (sess->vm) {
            if (strstr(payload, "\"key\":\"Up\"")) dvd_vm_bridge_up_button(sess->vm);
            else if (strstr(payload, "\"key\":\"Down\"")) dvd_vm_bridge_down_button(sess->vm);
            else if (strstr(payload, "\"key\":\"Left\"")) dvd_vm_bridge_left_button(sess->vm);
            else if (strstr(payload, "\"key\":\"Right\"")) dvd_vm_bridge_right_button(sess->vm);
            else if (strstr(payload, "\"key\":\"Activate\"")) dvd_vm_bridge_activate_button(sess->vm);
            else if (strstr(payload, "\"key\":\"Menu\"")) dvd_vm_bridge_menu(sess->vm);
            else if (strstr(payload, "\"key\":\"Escape\"")) dvd_vm_bridge_escape(sess->vm);
            else if (strstr(payload, "\"key\":\"TogglePause\"")) dvd_vm_bridge_toggle_pause(sess->vm);
            else if (strstr(payload, "\"key\":\"PrevChapter\"")) dvd_vm_bridge_prev_chapter(sess->vm);
            else if (strstr(payload, "\"key\":\"NextChapter\"")) dvd_vm_bridge_next_chapter(sess->vm);
            else if (strstr(payload, "\"key\":\"CycleSubtitles\"")) dvd_vm_bridge_cycle_subtitles(sess->vm);
            else if (strstr(payload, "\"key\":\"CycleAudio\"")) dvd_vm_bridge_cycle_audio(sess->vm);
            else if (strstr(payload, "\"mouse_move\"")) {
                int x = 0, y = 0;
                char *px = strstr(payload, "\"x\":");
                char *py = strstr(payload, "\"y\":");
                if (px && py) {
                    x = atoi(px + 4);
                    y = atoi(py + 4);
                    dvd_vm_bridge_mouse_move(sess->vm, x, y);
                }
            } else if (strstr(payload, "\"mouse_click\"")) {
                int x = 0, y = 0;
                char *px = strstr(payload, "\"x\":");
                char *py = strstr(payload, "\"y\":");
                if (px && py) {
                    x = atoi(px + 4);
                    y = atoi(py + 4);
                    dvd_vm_bridge_mouse_click(sess->vm, x, y);
                }
            } else {
                handle_client_playback_report(sess, payload);
            }
        }
        pthread_mutex_unlock(&sess->lock);

        free(payload);
    }

    wc->active = false;
    pthread_mutex_lock(&sess->ws_lock);
    sess->client_count--;
    sess->last_active_time = time(NULL);
    bool should_pause = (sess->client_count <= 0);
    pthread_mutex_unlock(&sess->ws_lock);

    if (should_pause) {
        pthread_mutex_lock(&sess->lock);
        if (sess->vm) dvd_vm_bridge_set_paused(sess->vm, true);
        pthread_mutex_unlock(&sess->lock);
    }
    close(client_fd);
}

static void send_http_error(int fd, int code, const char *msg) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: text/plain\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n\r\n%s",
             code, msg, strlen(msg), msg);
    send(fd, buf, strlen(buf), MSG_NOSIGNAL);
}

static void handle_http_range_file(int client_fd, const char *file_path, const char *range_header) {
    struct stat st;
    if (stat(file_path, &st) != 0 || st.st_size == 0) {
        send_http_error(client_fd, 404, "Not Found");
        return;
    }

    uint64_t total = (uint64_t)st.st_size;
    uint64_t start = 0, end = total - 1;
    bool partial = false;

    if (range_header && strncmp(range_header, "bytes=", 6) == 0) {
        const char *r = range_header + 6;
        char *dash = strchr(r, '-');
        if (dash) {
            partial = true;
            if (dash == r) {
                uint64_t suffix = strtoull(dash + 1, NULL, 10);
                start = (suffix >= total) ? 0 : (total - suffix);
                end = total - 1;
            } else {
                start = strtoull(r, NULL, 10);
                if (*(dash + 1) != '\0') {
                    end = strtoull(dash + 1, NULL, 10);
                } else {
                    end = total - 1;
                }
            }
        }
    }

    if (start > end || end >= total) {
        char err[256];
        snprintf(err, sizeof(err),
                 "HTTP/1.1 416 Range Not Satisfiable\r\n"
                 "Content-Range: bytes */%llu\r\n\r\n",
                 (unsigned long long)total);
        send(client_fd, err, strlen(err), MSG_NOSIGNAL);
        return;
    }

    FILE *f = fopen(file_path, "rb");
    if (!f) {
        send_http_error(client_fd, 500, "Internal Server Error");
        return;
    }

    uint64_t content_length = end - start + 1;
    char header[512];

    if (partial) {
        snprintf(header, sizeof(header),
                 "HTTP/1.1 206 Partial Content\r\n"
                 "Content-Type: video/mp4\r\n"
                 "Content-Range: bytes %llu-%llu/%llu\r\n"
                 "Content-Length: %llu\r\n"
                 "Accept-Ranges: bytes\r\n"
                 "Connection: keep-alive\r\n\r\n",
                 (unsigned long long)start,
                 (unsigned long long)end,
                 (unsigned long long)total,
                 (unsigned long long)content_length);
    } else {
        snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: video/mp4\r\n"
                 "Content-Length: %llu\r\n"
                 "Accept-Ranges: bytes\r\n"
                 "Connection: keep-alive\r\n\r\n",
                 (unsigned long long)total);
    }

    if (send(client_fd, header, strlen(header), MSG_NOSIGNAL) < 0) {
        fclose(f);
        return;
    }

    fseeko(f, (off_t)start, SEEK_SET);
    uint8_t buf[CHUNK_SIZE];
    uint64_t remaining = content_length;

    while (remaining > 0) {
        size_t to_read = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : (size_t)remaining;
        size_t n = fread(buf, 1, to_read, f);
        if (n == 0) break;
        ssize_t sent = send(client_fd, buf, n, MSG_NOSIGNAL);
        if (sent <= 0) break;
        remaining -= (uint64_t)sent;
    }

    fclose(f);
}

/* =========================================================================
 * Built-in DVD Import Pipeline & Helpers
 * ========================================================================= */

static void url_decode(const char *src, char *dst, size_t dst_len) {
    if (!src || !dst || dst_len == 0) return;
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dst_len; i++) {
        if (src[i] == '%' &&
            isxdigit((unsigned char)src[i + 1]) &&
            isxdigit((unsigned char)src[i + 2])) {
            char hex[3] = { src[i + 1], src[i + 2], '\0' };
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (src[i] == '+') {
            dst[j++] = ' ';
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

static bool extract_query_param(const char *uri, const char *key, char *out, size_t out_len) {
    if (!uri || !key || !out || out_len == 0) return false;
    out[0] = '\0';
    const char *q = strchr(uri, '?');
    if (!q) return false;
    const char *p = q + 1;
    size_t klen = strlen(key);
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t plen = amp ? (size_t)(amp - p) : strlen(p);
        if (plen > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            char encoded[1024];
            size_t vlen = plen - (klen + 1);
            if (vlen >= sizeof(encoded)) vlen = sizeof(encoded) - 1;
            memcpy(encoded, p + klen + 1, vlen);
            encoded[vlen] = '\0';
            url_decode(encoded, out, out_len);
            return true;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}

static void handle_client_request(web_server_t *s, int client_fd) {
    char req[4096];
    ssize_t n = recv(client_fd, req, sizeof(req) - 1, 0);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    req[n] = '\0';

    char method[16], uri[2048];
    if (sscanf(req, "%15s %2047s", method, uri) != 2) {
        send_http_error(client_fd, 400, "Bad Request");
        close(client_fd);
        return;
    }

    /* Extract query parameters */
    char session_param[64] = {0};
    char *q_sess = strstr(uri, "session=");
    if (q_sess) {
        strncpy(session_param, q_sess + 8, sizeof(session_param) - 1);
        char *amp = strchr(session_param, '&');
        if (amp) *amp = '\0';
    }

    char range_val[256] = {0};
    char *range_hdr = strcasestr(req, "\r\nRange: ");
    if (range_hdr) {
        char *end = strstr(range_hdr + 9, "\r\n");
        if (end) {
            size_t rlen = end - (range_hdr + 9);
            if (rlen < sizeof(range_val)) {
                strncpy(range_val, range_hdr + 9, rlen);
            }
        }
    }

    char ws_key[128] = {0};
    char *ws_hdr = strcasestr(req, "Sec-WebSocket-Key: ");
    if (ws_hdr) {
        char *end = strstr(ws_hdr + 19, "\r\n");
        if (end) {
            size_t klen = end - (ws_hdr + 19);
            if (klen < sizeof(ws_key)) {
                strncpy(ws_key, ws_hdr + 19, klen);
            }
        }
    }

    /* Route: WebSocket upgrade */
    if (strncmp(uri, "/ws", 3) == 0 && ws_key[0]) {
        handle_client_websocket(s, client_fd, ws_key, session_param[0] ? session_param : NULL);
        return;
    }

    /* Route: Home / Player SPA */
    if (strcmp(uri, "/") == 0 || strcmp(uri, "/index.html") == 0 || strncmp(uri, "/player", 7) == 0) {
        char header[256];
        size_t hlen = sizeof(TINYDVD_INDEX_HTML) - 1;
        snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/html; charset=utf-8\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n\r\n",
                 hlen);
        send(client_fd, header, strlen(header), MSG_NOSIGNAL);
        send(client_fd, TINYDVD_INDEX_HTML, hlen, MSG_NOSIGNAL);
        close(client_fd);
        return;
    }

    /* Route: /api/library */
    if (strncmp(uri, "/api/library", 12) == 0) {
        tdvd_library_rescan(&s->lib);

        char json[65536];
        pthread_mutex_lock(&s->lib.catalog_lock);
        int pos = snprintf(json, sizeof(json), "{\"is_directory\":%s,\"items\":[", s->lib.is_library ? "true" : "false");

        for (int i = 0; i < s->lib.catalog_count; i++) {
            catalog_item_t *item = &s->lib.catalog[i];
            char e_title[512] = {0}, e_dir[256] = {0}, e_cast[2048] = {0}, e_gen[256] = {0}, e_ov[2048] = {0};
            json_escape_copy(item->title, e_title, sizeof(e_title));
            json_escape_copy(item->director, e_dir, sizeof(e_dir));
            json_escape_copy(item->cast, e_cast, sizeof(e_cast));
            json_escape_copy(item->genres, e_gen, sizeof(e_gen));
            json_escape_copy(item->overview, e_ov, sizeof(e_ov));

            char elem[6144];
            snprintf(elem, sizeof(elem),
                     "%s{\"id\":\"%s\",\"title\":\"%s\",\"size_bytes\":%llu,\"size_str\":\"%s\",\"has_thumbnail\":%s,"
                     "\"year\":%d,\"director\":\"%s\",\"cast\":\"%s\",\"genres\":\"%s\",\"overview\":\"%s\"}",
                     (i > 0) ? "," : "",
                     item->id, e_title,
                     (unsigned long long)item->size_bytes, item->size_str,
                     item->has_thumbnail ? "true" : "false",
                     item->year, e_dir, e_cast, e_gen, e_ov);
            int elen = strlen(elem);
            if (pos + elen + 10 < (int)sizeof(json)) {
                strcpy(json + pos, elem);
                pos += elen;
            }
        }
        pthread_mutex_unlock(&s->lib.catalog_lock);
        strcpy(json + pos, "]}");

        char header[256];
        size_t jlen = strlen(json);
        snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n\r\n",
                 jlen);
        send(client_fd, header, strlen(header), MSG_NOSIGNAL);
        send(client_fd, json, jlen, MSG_NOSIGNAL);
        close(client_fd);
        return;
    }

    /* Route: /api/import/status */
    if (strncmp(uri, "/api/import/status", 18) == 0) {
        char default_src[512] = {0};
        char default_title[256] = {0};
        tdvd_library_detect_default_source(default_src, sizeof(default_src), default_title, sizeof(default_title));

        import_job_t *job = &s->lib.import_job;
        char *json = (char *)malloc(65536);
        if (!json) {
            send_http_error(client_fd, 500, "Out of Memory");
            close(client_fd);
            return;
        }

        pthread_mutex_lock(&job->lock);
        const char *st_name = tdvd_import_state_name(job->state);
        bool active = job->active;
        int elapsed = (job->start_time > 0) ? (int)(time(NULL) - job->start_time) : 0;

        char e_def_src[1024] = {0}, e_def_title[512] = {0};
        char e_src[1024] = {0}, e_title[512] = {0};
        char e_stage[512] = {0}, e_err[1024] = {0};
        char e_res_id[256] = {0}, e_res_title[512] = {0};

        json_escape_copy(default_src, e_def_src, sizeof(e_def_src));
        json_escape_copy(default_title, e_def_title, sizeof(e_def_title));
        json_escape_copy(job->source, e_src, sizeof(e_src));
        json_escape_copy(job->title, e_title, sizeof(e_title));
        json_escape_copy(job->stage_msg, e_stage, sizeof(e_stage));
        json_escape_copy(job->error_msg, e_err, sizeof(e_err));
        json_escape_copy(job->result_disc_id, e_res_id, sizeof(e_res_id));
        json_escape_copy(job->result_title, e_res_title, sizeof(e_res_title));

        int pos = snprintf(json, 65536,
                           "{\"state\":\"%s\",\"active\":%s,\"elapsed_sec\":%d,"
                           "\"default_source\":\"%s\",\"default_title\":\"%s\","
                           "\"source\":\"%s\",\"title\":\"%s\","
                           "\"stage_msg\":\"%s\",\"error_msg\":\"%s\","
                           "\"result_disc_id\":\"%s\",\"result_title\":\"%s\",\"logs\":[",
                           st_name, active ? "true" : "false", elapsed,
                           e_def_src, e_def_title,
                           e_src, e_title,
                           e_stage, e_err,
                           e_res_id, e_res_title);

        for (int i = 0; i < job->log_count; i++) {
            char e_line[IMPORT_LOG_LINE_LEN * 2] = {0};
            json_escape_copy(job->log_lines[i], e_line, sizeof(e_line));
            int need = (int)strlen(e_line) + 4;
            if (pos + need + 16 < 65536) {
                pos += snprintf(json + pos, 65536 - pos, "%s\"%s\"", (i > 0) ? "," : "", e_line);
            }
        }
        pthread_mutex_unlock(&job->lock);
        snprintf(json + pos, 65536 - pos, "]}");

        char header[256];
        size_t jlen = strlen(json);
        snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %zu\r\n"
                 "Cache-Control: no-store\r\n"
                 "Connection: close\r\n\r\n",
                 jlen);
        send(client_fd, header, strlen(header), MSG_NOSIGNAL);
        send(client_fd, json, jlen, MSG_NOSIGNAL);
        free(json);
        close(client_fd);
        return;
    }

    /* Route: /api/import/start */
    if (strncmp(uri, "/api/import/start", 17) == 0) {
        char source_param[512] = {0};
        char title_param[256] = {0};
        char skip_param[32] = {0};
        extract_query_param(uri, "source", source_param, sizeof(source_param));
        extract_query_param(uri, "title", title_param, sizeof(title_param));
        extract_query_param(uri, "skip_enrichment", skip_param, sizeof(skip_param));

        bool skip_enrich = (strcmp(skip_param, "1") == 0 ||
                            strcasecmp(skip_param, "true") == 0 ||
                            strcasecmp(skip_param, "yes") == 0);

        char err_buf[256] = {0};
        if (tdvd_library_import_start(&s->lib, source_param, title_param, skip_enrich, err_buf, sizeof(err_buf))) {
            const char *ok_json = "{\"status\":\"started\"}";
            char header[256];
            snprintf(header, sizeof(header),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n\r\n",
                     strlen(ok_json));
            send(client_fd, header, strlen(header), MSG_NOSIGNAL);
            send(client_fd, ok_json, strlen(ok_json), MSG_NOSIGNAL);
        } else {
            const char *busy_json = "{\"status\":\"error\",\"error\":\"An import job is already in progress\"}";
            char header[256];
            snprintf(header, sizeof(header),
                     "HTTP/1.1 409 Conflict\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n\r\n",
                     strlen(busy_json));
            send(client_fd, header, strlen(header), MSG_NOSIGNAL);
            send(client_fd, busy_json, strlen(busy_json), MSG_NOSIGNAL);
        }
        close(client_fd);
        return;
    }

    /* Route: /api/import/cancel */
    if (strncmp(uri, "/api/import/cancel", 18) == 0) {
        tdvd_library_import_cancel(&s->lib);

        const char *resp_json = "{\"status\":\"cancelling\"}";
        char header[256];
        snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n\r\n",
                 strlen(resp_json));
        send(client_fd, header, strlen(header), MSG_NOSIGNAL);
        send(client_fd, resp_json, strlen(resp_json), MSG_NOSIGNAL);
        close(client_fd);
        return;
    }

    /* Route: /api/thumbnail?disc=<id> or /api/thumbnail/<id> */
    if (strncmp(uri, "/api/thumbnail", 14) == 0) {
        char disc_id[128] = {0};
        char *q = strstr(uri, "disc=");
        if (q) {
            strncpy(disc_id, q + 5, sizeof(disc_id) - 1);
            char *amp = strchr(disc_id, '&');
            if (amp) *amp = '\0';
        } else if (strlen(uri) > 15) {
            strncpy(disc_id, uri + 15, sizeof(disc_id) - 1);
        }

        char disc_path[1024] = {0};
        pthread_mutex_lock(&s->lib.catalog_lock);
        snprintf(disc_path, sizeof(disc_path), "%s", s->lib.default_disc_path);
        if (s->lib.is_library && disc_id[0]) {
            for (int i = 0; i < s->lib.catalog_count; i++) {
                if (strcmp(s->lib.catalog[i].id, disc_id) == 0) {
                    snprintf(disc_path, sizeof(disc_path), "%s", s->lib.catalog[i].path);
                    break;
                }
            }
        }
        pthread_mutex_unlock(&s->lib.catalog_lock);

        if (tdvd_library_is_tdvd_file(disc_path)) {
            tdvd_archive_t *arc = tdvd_archive_open(disc_path);
            uint64_t toff = 0, tsz = 0;
            if (arc && tdvd_archive_find(arc, "thumbnail.png", &toff, &tsz)) {
                FILE *f = fopen(disc_path, "rb");
                if (f) {
                    char header[256];
                    snprintf(header, sizeof(header),
                             "HTTP/1.1 200 OK\r\n"
                             "Content-Type: image/png\r\n"
                             "Content-Length: %llu\r\n"
                             "Cache-Control: public, max-age=86400\r\n"
                             "Connection: close\r\n\r\n",
                             (unsigned long long)tsz);
                    send(client_fd, header, strlen(header), MSG_NOSIGNAL);
                    fseeko(f, toff, SEEK_SET);
                    uint8_t buf[CHUNK_SIZE];
                    uint64_t rem = tsz;
                    while (rem > 0) {
                        size_t r = (rem > CHUNK_SIZE) ? CHUNK_SIZE : (size_t)rem;
                        size_t nread = fread(buf, 1, r, f);
                        if (nread == 0) break;
                        send(client_fd, buf, nread, MSG_NOSIGNAL);
                        rem -= nread;
                    }
                    fclose(f);
                    tdvd_archive_close(arc);
                    close(client_fd);
                    return;
                }
            }
            if (arc) tdvd_archive_close(arc);
        }
        send_http_error(client_fd, 404, "Thumbnail Not Found");
        close(client_fd);
        return;
    }

    /* Route: /api/play?session=<id>&disc=<disc_id> */
    if (strncmp(uri, "/api/play", 9) == 0) {
        char disc_id[128] = {0};
        char *q = strstr(uri, "disc=");
        if (q) {
            strncpy(disc_id, q + 5, sizeof(disc_id) - 1);
            char *amp = strchr(disc_id, '&');
            if (amp) *amp = '\0';
        }

        char new_path[1024] = {0};
        char new_title[256] = {0};
        pthread_mutex_lock(&s->lib.catalog_lock);
        for (int i = 0; i < s->lib.catalog_count; i++) {
            if (strcmp(s->lib.catalog[i].id, disc_id) == 0) {
                snprintf(new_path, sizeof(new_path), "%s", s->lib.catalog[i].path);
                snprintf(new_title, sizeof(new_title), "%s", s->lib.catalog[i].title);
                break;
            }
        }
        pthread_mutex_unlock(&s->lib.catalog_lock);

        if (new_path[0]) {
            web_session_t *sess = get_or_create_session(s, session_param[0] ? session_param : NULL, new_path, new_title);
            if (sess) {
                session_load_movie(sess, new_path, new_title);
                char resp[256];
                snprintf(resp, sizeof(resp),
                         "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n{\"status\":\"ok\",\"session_id\":\"%s\"}",
                         strlen(sess->id) + 29, sess->id);
                send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);
                close(client_fd);
                return;
            }
        }
        send_http_error(client_fd, 404, "Movie Not Found");
        close(client_fd);
        return;
    }

    /* Route: /api/media/<filename> */
    if (strncmp(uri, "/api/media/", 11) == 0) {
        char base[128] = {0};
        strncpy(base, uri + 11, sizeof(base) - 1);
        char *q = strchr(base, '?');
        if (q) *q = '\0';
        char *dot = strrchr(base, '.');
        if (dot) *dot = '\0';

        char disc_param[128] = {0};
        char *q_disc = strstr(uri, "disc=");
        if (q_disc) {
            strncpy(disc_param, q_disc + 5, sizeof(disc_param) - 1);
            char *amp = strchr(disc_param, '&');
            if (amp) *amp = '\0';
        }

        char target_disc_path[1024] = {0};
        char target_disc_id[128] = {0};
        pthread_mutex_lock(&s->lib.catalog_lock);
        snprintf(target_disc_path, sizeof(target_disc_path), "%s", s->lib.default_disc_path);
        snprintf(target_disc_id, sizeof(target_disc_id), "%s", s->lib.default_disc_id);
        if (disc_param[0]) {
            for (int i = 0; i < s->lib.catalog_count; i++) {
                if (strcmp(s->lib.catalog[i].id, disc_param) == 0) {
                    snprintf(target_disc_path, sizeof(target_disc_path), "%s", s->lib.catalog[i].path);
                    snprintf(target_disc_id, sizeof(target_disc_id), "%s", s->lib.catalog[i].id);
                    break;
                }
            }
        }
        pthread_mutex_unlock(&s->lib.catalog_lock);

        char mp4_path[1024];
        if (ensure_remuxed_mp4(s, target_disc_path, target_disc_id, base, mp4_path, sizeof(mp4_path))) {
            handle_http_range_file(client_fd, mp4_path, range_val[0] ? range_val : NULL);
        } else {
            send_http_error(client_fd, 404, "Media Stream Not Found");
        }
        close(client_fd);
        return;
    }

    send_http_error(client_fd, 404, "Not Found");
    close(client_fd);
}

/* =========================================================================
 * Connection Accept Thread & Server Main Loop
 * ========================================================================= */

typedef struct client_task {
    web_server_t *server;
    int client_fd;
} client_task_t;

static void *client_worker_thread(void *arg) {
    client_task_t *task = (client_task_t *)arg;
    handle_client_request(task->server, task->client_fd);
    free(task);
    return NULL;
}

static void *accept_loop(void *arg) {
    web_server_t *s = (web_server_t *)arg;

    while (s->running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(s->listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }

        client_task_t *task = (client_task_t *)malloc(sizeof(client_task_t));
        task->server = s;
        task->client_fd = client_fd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_worker_thread, task) == 0) {
            pthread_detach(tid);
        } else {
            close(client_fd);
            free(task);
        }
    }
    return NULL;
}

/* =========================================================================
 * Catalog Scanner & Public Lifecycle API
 * ========================================================================= */

static void json_escape_copy(const char *in, char *out, size_t out_len) {
    if (!in || !out || out_len == 0) return;
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 2 < out_len; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') {
            out[j++] = '\\';
            out[j++] = c;
        } else if (c == '\n' || c == '\r' || c == '\t') {
            out[j++] = ' ';
        } else if ((unsigned char)c >= 32) {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

web_server_t *web_server_start(const web_server_config_t *config) {
    if (!config || !config->target_path) return NULL;

    signal(SIGPIPE, SIG_IGN);

    web_server_t *s = (web_server_t *)calloc(1, sizeof(web_server_t));
    s->port = config->port ? config->port : 8080;
    strncpy(s->bind_addr, config->bind_addr ? config->bind_addr : "0.0.0.0", sizeof(s->bind_addr) - 1);

    pthread_mutex_init(&s->sessions_lock, NULL);
    tdvd_library_init(&s->lib, config->target_path);

    /* Start listening socket */
    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0) {
        perror("socket");
        tdvd_library_destroy(&s->lib);
        pthread_mutex_destroy(&s->sessions_lock);
        free(s);
        return NULL;
    }

    int opt = 1;
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)s->port);
    inet_pton(AF_INET, s->bind_addr, &sin.sin_addr);

    if (bind(s->listen_fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        fprintf(stderr, "[web_server] Failed to bind to %s:%d (errno: %s)\n", s->bind_addr, s->port, strerror(errno));
        close(s->listen_fd);
        tdvd_library_destroy(&s->lib);
        pthread_mutex_destroy(&s->sessions_lock);
        free(s);
        return NULL;
    }

    if (listen(s->listen_fd, 64) < 0) {
        perror("listen");
        close(s->listen_fd);
        tdvd_library_destroy(&s->lib);
        pthread_mutex_destroy(&s->sessions_lock);
        free(s);
        return NULL;
    }

    s->running = true;

    if (pthread_create(&s->accept_thread, NULL, accept_loop, s) != 0) {
        s->running = false;
        close(s->listen_fd);
        tdvd_library_destroy(&s->lib);
        pthread_mutex_destroy(&s->sessions_lock);
        free(s);
        return NULL;
    }

    printf("\n=======================================================\n");
    printf("  TinyDVD Web Player Running!\n");
    printf("  URL: http://localhost:%d\n", s->port);
    printf("  Mode: %s (%d movie%s available)\n",
           s->lib.is_library ? "Library Catalog" : "Direct Cinema Player",
           s->lib.catalog_count, s->lib.catalog_count == 1 ? "" : "s");
    printf("  Multi-Session Support: Enabled (isolated playback per tab/window)\n");
    printf("=======================================================\n\n");

    return s;
}

void web_server_run(web_server_t *s) {
    if (!s) return;
    while (s->running) {
        sleep(5);

        /* Periodically prune dead sessions that have been idle with 0 clients for > SESSION_TIMEOUT_SEC */
        pthread_mutex_lock(&s->sessions_lock);
        web_session_t **curr = &s->sessions;
        time_t now = time(NULL);
        while (*curr) {
            web_session_t *entry = *curr;
            if (entry->client_count == 0 && (now - entry->last_active_time) > SESSION_TIMEOUT_SEC) {
                *curr = entry->next;
                fprintf(stderr, "[web_server] Pruning expired idle session %s (%s)\n", entry->id, entry->disc_title);
                session_destroy(entry);
            } else {
                curr = &entry->next;
            }
        }
        pthread_mutex_unlock(&s->sessions_lock);
    }
}

void web_server_stop(web_server_t *s) {
    if (!s || !s->running) return;
    s->running = false;
    shutdown(s->listen_fd, SHUT_RDWR);
    close(s->listen_fd);
    pthread_join(s->accept_thread, NULL);
}

void web_server_destroy(web_server_t *s) {
    if (!s) return;
    web_server_stop(s);

    tdvd_library_destroy(&s->lib);

    pthread_mutex_lock(&s->sessions_lock);
    web_session_t *sess = s->sessions;
    while (sess) {
        web_session_t *next = sess->next;
        session_destroy(sess);
        sess = next;
    }
    s->sessions = NULL;
    pthread_mutex_unlock(&s->sessions_lock);

    pthread_mutex_destroy(&s->sessions_lock);
    free(s);
}

void web_server_handle_client(web_server_t *s, int client_fd) {
    if (!s || client_fd < 0) return;
    handle_client_request(s, client_fd);
}
