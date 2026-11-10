#include "mpv_controller.h"
#include <mpv/stream_cb.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <unistd.h>

typedef struct {
    int      fd;
    uint64_t base_offset;
    uint64_t size;
    uint64_t pos;
} tdvd_mpv_stream_t;

static int64_t tdvd_stream_read(void *cookie, char *buf, uint64_t nbytes) {
    tdvd_mpv_stream_t *s = (tdvd_mpv_stream_t *)cookie;
    if (!s || s->pos >= s->size) return 0;
    uint64_t avail = s->size - s->pos;
    if (nbytes > avail) nbytes = avail;
    ssize_t n = pread(s->fd, buf, (size_t)nbytes, (off_t)(s->base_offset + s->pos));
    if (n <= 0) return n < 0 ? -1 : 0;
    s->pos += (uint64_t)n;
    return (int64_t)n;
}

static int64_t tdvd_stream_seek(void *cookie, int64_t offset) {
    tdvd_mpv_stream_t *s = (tdvd_mpv_stream_t *)cookie;
    if (!s || offset < 0 || (uint64_t)offset > s->size) return MPV_ERROR_GENERIC;
    s->pos = (uint64_t)offset;
    return offset;
}

static int64_t tdvd_stream_size(void *cookie) {
    tdvd_mpv_stream_t *s = (tdvd_mpv_stream_t *)cookie;
    return s ? (int64_t)s->size : MPV_ERROR_GENERIC;
}

static void tdvd_stream_close(void *cookie) {
    tdvd_mpv_stream_t *s = (tdvd_mpv_stream_t *)cookie;
    if (!s) return;
    if (s->fd >= 0) close(s->fd);
    free(s);
}

static int tdvd_stream_open(void *user_data, char *uri, mpv_stream_cb_info *info) {
    (void)user_data;
    if (!uri || !info) return MPV_ERROR_LOADING_FAILED;
    const char *p = uri;
    if (strncmp(p, "tdvd://", 7) == 0) p += 7;

    char *end1 = NULL;
    uint64_t base_offset = strtoull(p, &end1, 10);
    if (!end1 || *end1 != ':') return MPV_ERROR_LOADING_FAILED;

    char *end2 = NULL;
    uint64_t slice_size = strtoull(end1 + 1, &end2, 10);
    if (!end2 || *end2 != ':') return MPV_ERROR_LOADING_FAILED;

    const char *file_path = end2 + 1;
    int fd = open(file_path, O_RDONLY);
    if (fd < 0) return MPV_ERROR_LOADING_FAILED;

    tdvd_mpv_stream_t *s = (tdvd_mpv_stream_t *)calloc(1, sizeof(tdvd_mpv_stream_t));
    if (!s) {
        close(fd);
        return MPV_ERROR_NOMEM;
    }
    s->fd = fd;
    s->base_offset = base_offset;
    s->size = slice_size;
    s->pos = 0;

    info->cookie   = s;
    info->read_fn  = tdvd_stream_read;
    info->seek_fn  = tdvd_stream_seek;
    info->size_fn  = tdvd_stream_size;
    info->close_fn = tdvd_stream_close;
    return 0;
}

struct mpv_controller {
    mpv_handle *mpv;
    mpv_render_context *render_ctx;
    mpv_update_callback_t on_update;
    void *user_data;
    atomic_bool needs_render;
    atomic_bool cached_paused;
    atomic_bool file_loaded;
    atomic_bool seek_in_flight;
    atomic_uint_least64_t cached_pts_nanos;
    atomic_int_least64_t pending_seek_nanos;
    atomic_int pending_aid;
    /* Display aspect of the current video (width / height) x 10000, 0 if unknown */
    atomic_int display_aspect_e4;
};

static void on_mpv_render_update_internal(void *ctx) {
    mpv_controller_t *ctrl = (mpv_controller_t *)ctx;
    if (ctrl) {
        atomic_store(&ctrl->needs_render, true);
        if (ctrl->on_update) {
            ctrl->on_update(ctrl->user_data);
        }
    }
}

static void send_seek_command_internal(mpv_handle *mpv, uint64_t pts_nanos) {
    char pts_str[64];
    snprintf(pts_str, sizeof(pts_str), "%.6f", (double)pts_nanos / 1e9);
    const char *args[] = {"seek", pts_str, "absolute+exact", NULL};
    mpv_command_async(mpv, 0, args);
}

static void send_aid_property_internal(mpv_handle *mpv, int aid) {
    if (aid <= 0) {
        const char *val = "no";
        mpv_set_property_async(mpv, 0, "aid", MPV_FORMAT_STRING, &val);
    } else {
        int64_t track_id = (int64_t)aid;
        mpv_set_property_async(mpv, 0, "aid", MPV_FORMAT_INT64, &track_id);
    }
}

mpv_controller_t *mpv_controller_init(void *(*get_proc_address)(void *, const char *),
                                     mpv_update_callback_t on_update,
                                     void *user_data) {
    mpv_controller_t *ctrl = (mpv_controller_t *)calloc(1, sizeof(mpv_controller_t));
    if (!ctrl) return NULL;

    ctrl->on_update = on_update;
    ctrl->user_data = user_data;
    atomic_init(&ctrl->needs_render, false);
    atomic_init(&ctrl->cached_paused, false);
    atomic_init(&ctrl->file_loaded, false);
    atomic_init(&ctrl->seek_in_flight, false);
    atomic_init(&ctrl->cached_pts_nanos, 0);
    atomic_init(&ctrl->pending_seek_nanos, -1);
    atomic_init(&ctrl->pending_aid, 1);
    atomic_init(&ctrl->display_aspect_e4, 0);

    ctrl->mpv = mpv_create();
    if (!ctrl->mpv) {
        fprintf(stderr, "[mpv_controller] Failed to create mpv handle\n");
        free(ctrl);
        return NULL;
    }

    /* Embed video inside our SDL/OpenGL render context instead of opening a second window */
    mpv_set_option_string(ctrl->mpv, "vo", "libmpv");
    mpv_set_option_string(ctrl->mpv, "hr-seek", "yes");
    mpv_set_option_string(ctrl->mpv, "keep-open", "yes");
    mpv_set_option_string(ctrl->mpv, "audio-pitch-correction", "no");
    mpv_set_option_string(ctrl->mpv, "terminal", "no");
    mpv_set_option_string(ctrl->mpv, "msg-level", "all=warn");

    int res = mpv_initialize(ctrl->mpv);
    if (res < 0) {
        fprintf(stderr, "[mpv_controller] Failed to initialize mpv: %s\n", mpv_error_string(res));
        mpv_destroy(ctrl->mpv);
        free(ctrl);
        return NULL;
    }

    /* Observe time-pos and pause asynchronously so background VM thread never blocks on mpv */
    mpv_observe_property(ctrl->mpv, 1, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(ctrl->mpv, 2, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(ctrl->mpv, 3, "video-params/aspect", MPV_FORMAT_DOUBLE);

    /* Register zero-copy tdvd:// byte-slice stream protocol for .tdvd single-file containers */
    mpv_stream_cb_add_ro(ctrl->mpv, "tdvd", ctrl, tdvd_stream_open);

    if (get_proc_address) {
        mpv_opengl_init_params gl_init = {
            .get_proc_address = get_proc_address,
            .get_proc_address_ctx = NULL
        };
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_OPENGL},
            {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init},
            {0}
        };

        res = mpv_render_context_create(&ctrl->render_ctx, ctrl->mpv, params);
        if (res < 0) {
            fprintf(stderr, "[mpv_controller] Warning: mpv_render_context_create failed (%s)\n",
                    mpv_error_string(res));
            ctrl->render_ctx = NULL;
        } else {
            mpv_render_context_set_update_callback(ctrl->render_ctx, on_mpv_render_update_internal, ctrl);
        }
    }

    return ctrl;
}

void mpv_controller_destroy(mpv_controller_t *ctrl) {
    if (!ctrl) return;
    if (ctrl->render_ctx) {
        mpv_render_context_set_update_callback(ctrl->render_ctx, NULL, NULL);
        mpv_render_context_free(ctrl->render_ctx);
        ctrl->render_ctx = NULL;
    }
    if (ctrl->mpv) {
        mpv_terminate_destroy(ctrl->mpv);
        ctrl->mpv = NULL;
    }
    free(ctrl);
}

int mpv_controller_load_file(mpv_controller_t *ctrl, const char *video_path, const char *audio_path) {
    if (!ctrl || !ctrl->mpv || !video_path) return -1;

    atomic_store(&ctrl->file_loaded, false);
    atomic_store(&ctrl->seek_in_flight, false);
    atomic_store(&ctrl->pending_seek_nanos, -1);
    atomic_store(&ctrl->cached_pts_nanos, 0);
    const char *args[] = {"loadfile", video_path, "replace", NULL};
    int err = mpv_command_async(ctrl->mpv, 0, args);
    if (err < 0) {
        fprintf(stderr, "[mpv_controller] Failed to queue loadfile %s: %s\n", video_path, mpv_error_string(err));
        return err;
    }

    if (audio_path && strlen(audio_path) > 0) {
        const char *audio_args[] = {"audio-add", audio_path, "cached", NULL};
        mpv_command_async(ctrl->mpv, 0, audio_args);
    }

    return 0;
}

void mpv_controller_set_audio_track(mpv_controller_t *ctrl, int aid) {
    if (!ctrl || !ctrl->mpv) return;
    atomic_store(&ctrl->pending_aid, aid);
    if (atomic_load(&ctrl->file_loaded)) {
        send_aid_property_internal(ctrl->mpv, aid);
    }
}

void mpv_controller_set_subtitle_track(mpv_controller_t *ctrl, int sid) {
    if (!ctrl || !ctrl->mpv) return;
    if (sid <= 0) {
        const char *val = "no";
        mpv_set_property_async(ctrl->mpv, 0, "sid", MPV_FORMAT_STRING, &val);
    } else {
        int64_t track_id = (int64_t)sid;
        mpv_set_property_async(ctrl->mpv, 0, "sid", MPV_FORMAT_INT64, &track_id);
    }
}

void mpv_controller_osd_message(mpv_controller_t *ctrl, const char *msg) {
    if (!ctrl || !ctrl->mpv || !msg) return;
    const char *args[] = {"show-text", msg, "2000", NULL};
    mpv_command_async(ctrl->mpv, 0, args);
}

void mpv_controller_seek_exact(mpv_controller_t *ctrl, uint64_t pts_nanos) {
    if (!ctrl || !ctrl->mpv) return;

    atomic_store(&ctrl->cached_pts_nanos, pts_nanos);
    atomic_store(&ctrl->seek_in_flight, true);
    if (atomic_load(&ctrl->file_loaded)) {
        atomic_store(&ctrl->pending_seek_nanos, -1);
        send_seek_command_internal(ctrl->mpv, pts_nanos);
    } else {
        /* File is currently loading asynchronously; defer seek until MPV_EVENT_FILE_LOADED */
        atomic_store(&ctrl->pending_seek_nanos, (int64_t)pts_nanos);
    }
}

void mpv_controller_pause(mpv_controller_t *ctrl, bool pause) {
    if (!ctrl || !ctrl->mpv) return;
    int p = pause ? 1 : 0;
    atomic_store(&ctrl->cached_paused, pause);
    mpv_set_property_async(ctrl->mpv, 0, "pause", MPV_FORMAT_FLAG, &p);
}

bool mpv_controller_is_paused(mpv_controller_t *ctrl) {
    if (!ctrl) return false;
    return atomic_load(&ctrl->cached_paused);
}

uint64_t mpv_controller_get_pts_nanos(mpv_controller_t *ctrl) {
    if (!ctrl) return 0;
    return atomic_load(&ctrl->cached_pts_nanos);
}

double mpv_controller_get_time_pos(mpv_controller_t *ctrl) {
    if (!ctrl) return 0.0;
    return (double)atomic_load(&ctrl->cached_pts_nanos) / 1e9;
}

void mpv_controller_render(mpv_controller_t *ctrl, int width, int height) {
    if (!ctrl || !ctrl->render_ctx) return;

    mpv_opengl_fbo fbo = {
        .fbo = 0,
        .w = width,
        .h = height,
        .internal_format = 0,
    };
    int flip_y = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {0}
    };
    mpv_render_context_render(ctrl->render_ctx, params);
    atomic_store(&ctrl->needs_render, false);
}

void mpv_controller_poll_events(mpv_controller_t *ctrl) {
    if (!ctrl || !ctrl->mpv) return;
    while (1) {
        mpv_event *event = mpv_wait_event(ctrl->mpv, 0);
        if (!event || event->event_id == MPV_EVENT_NONE) break;
        if (event->event_id == MPV_EVENT_FILE_LOADED) {
            atomic_store(&ctrl->file_loaded, true);
            int aid = atomic_load(&ctrl->pending_aid);
            if (aid > 0) {
                send_aid_property_internal(ctrl->mpv, aid);
            }
            int64_t seek_ns = atomic_exchange(&ctrl->pending_seek_nanos, -1);
            if (seek_ns >= 0) {
                atomic_store(&ctrl->seek_in_flight, true);
                send_seek_command_internal(ctrl->mpv, (uint64_t)seek_ns);
                int p = atomic_load(&ctrl->cached_paused) ? 1 : 0;
                mpv_set_property_async(ctrl->mpv, 0, "pause", MPV_FORMAT_FLAG, &p);
            }
        } else if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) {
            if (atomic_load(&ctrl->file_loaded) && atomic_load(&ctrl->pending_seek_nanos) < 0) {
                atomic_store(&ctrl->seek_in_flight, false);
            }
        } else if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            mpv_event_property *prop = (mpv_event_property *)event->data;
            if (!prop || !prop->name || !prop->data) continue;
            if (strcmp(prop->name, "time-pos") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
                double sec = *(double *)prop->data;
                if (sec < 0.0) sec = 0.0;
                /* Only overwrite cached_pts_nanos once file load and any pending seek have completed */
                if (atomic_load(&ctrl->file_loaded) &&
                    !atomic_load(&ctrl->seek_in_flight) &&
                    atomic_load(&ctrl->pending_seek_nanos) < 0) {
                    atomic_store(&ctrl->cached_pts_nanos, (uint64_t)(sec * 1e9));
                }
            } else if (strcmp(prop->name, "pause") == 0 && prop->format == MPV_FORMAT_FLAG) {
                int p = *(int *)prop->data;
                atomic_store(&ctrl->cached_paused, p != 0);
            } else if (strcmp(prop->name, "video-params/aspect") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
                double aspect = *(double *)prop->data;
                if (aspect > 0.5 && aspect < 4.0) {
                    atomic_store(&ctrl->display_aspect_e4, (int)(aspect * 10000.0 + 0.5));
                }
            }
        }
    }
}

bool mpv_controller_needs_render(mpv_controller_t *ctrl) {
    if (!ctrl) return false;
    if (ctrl->render_ctx) {
        uint64_t flags = mpv_render_context_update(ctrl->render_ctx);
        if (flags & MPV_RENDER_UPDATE_FRAME) {
            atomic_store(&ctrl->needs_render, true);
        }
    }
    return atomic_load(&ctrl->needs_render);
}

float mpv_controller_get_display_aspect(mpv_controller_t *ctrl) {
    int e4 = ctrl ? atomic_load(&ctrl->display_aspect_e4) : 0;
    return e4 > 0 ? (float)e4 / 10000.0f : 16.0f / 9.0f;
}

mpv_handle *mpv_controller_get_handle(mpv_controller_t *ctrl) {
    return ctrl ? ctrl->mpv : NULL;
}
