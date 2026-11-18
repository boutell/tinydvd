#include <SDL2/SDL.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <ctype.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "mvob_parser.h"
#include "mpv_controller.h"
#include "spu_compositor.h"
#include "spu_png.h"
#include "dvd_vm_bridge.h"
#include "player_adapter.h"
#include "tdvd_archive.h"
#include "library.h"
#include "ui/native_ui.h"

#define TINYDVD_VERSION "1.0.0"
#define WAKEUP_EVENT (SDL_USEREVENT + 1)
#define CURSOR_IDLE_MS 2000

/* =========================================================================
 * Part A: Desktop Library Bridge (backed by Python TinyDVDLibrary)
 * ========================================================================= */

typedef int  (*py_read_thumb_cb_t)(const char *disc_path, const uint8_t **out_ptr, size_t *out_sz);
typedef void (*py_detect_source_cb_t)(char *out_src, size_t src_len, char *out_label, size_t label_len);
typedef int  (*py_import_start_cb_t)(const char *source, const char *title, int skip_enrich, char *err_buf, size_t err_len);
typedef void (*py_import_cancel_cb_t)(void);
typedef void (*py_set_dir_cb_t)(const char *dir_path);

static tdvd_library_t g_desktop_lib;
static bool g_desktop_lib_inited = false;

static py_read_thumb_cb_t    g_cb_read_thumb    = NULL;
static py_detect_source_cb_t g_cb_detect_source = NULL;
static py_import_start_cb_t  g_cb_import_start  = NULL;
static py_import_cancel_cb_t g_cb_import_cancel = NULL;
static py_set_dir_cb_t       g_cb_set_dir       = NULL;

static void ensure_desktop_lib_inited(void) {
    if (g_desktop_lib_inited) return;
    memset(&g_desktop_lib, 0, sizeof(g_desktop_lib));
    pthread_mutex_init(&g_desktop_lib.catalog_lock, NULL);
    pthread_cond_init(&g_desktop_lib.catalog_cond, NULL);
    pthread_mutex_init(&g_desktop_lib.import_job.lock, NULL);
    g_desktop_lib.import_job.state = IMPORT_IDLE;
    g_desktop_lib.watch_wake_pipe[0] = -1;
    g_desktop_lib.watch_wake_pipe[1] = -1;
    g_desktop_lib_inited = true;
}

void tdvd_library_bind_python_callbacks(
    py_read_thumb_cb_t read_thumb,
    py_detect_source_cb_t detect_source,
    py_import_start_cb_t import_start,
    py_import_cancel_cb_t import_cancel,
    py_set_dir_cb_t set_dir
) {
    ensure_desktop_lib_inited();
    g_cb_read_thumb = read_thumb;
    g_cb_detect_source = detect_source;
    g_cb_import_start = import_start;
    g_cb_import_cancel = import_cancel;
    g_cb_set_dir = set_dir;
}

void tdvd_desktop_set_library_mode(const char *target_path, int is_library) {
    ensure_desktop_lib_inited();
    pthread_mutex_lock(&g_desktop_lib.catalog_lock);
    snprintf(g_desktop_lib.target_path, sizeof(g_desktop_lib.target_path), "%s",
             target_path ? target_path : "");
    g_desktop_lib.is_library = (is_library != 0);
    pthread_mutex_unlock(&g_desktop_lib.catalog_lock);
}

void tdvd_desktop_sync_catalog_begin(void) {
    ensure_desktop_lib_inited();
    pthread_mutex_lock(&g_desktop_lib.catalog_lock);
    g_desktop_lib.catalog_count = 0;
}

void tdvd_desktop_sync_catalog_add(
    const char *id, const char *title, const char *path,
    uint64_t size_bytes, const char *size_str,
    int has_thumbnail, int year,
    const char *director, const char *cast,
    const char *genres, const char *overview
) {
    if (!g_desktop_lib_inited) return;
    if (g_desktop_lib.catalog_count >= g_desktop_lib.catalog_cap) {
        int new_cap = g_desktop_lib.catalog_cap ? (g_desktop_lib.catalog_cap * 2) : 16;
        catalog_item_t *grown = (catalog_item_t *)realloc(
            g_desktop_lib.catalog, (size_t)new_cap * sizeof(catalog_item_t));
        if (!grown) return;
        g_desktop_lib.catalog = grown;
        g_desktop_lib.catalog_cap = new_cap;
    }

    catalog_item_t *item = &g_desktop_lib.catalog[g_desktop_lib.catalog_count++];
    memset(item, 0, sizeof(*item));
    snprintf(item->id, sizeof(item->id), "%s", id ? id : "");
    snprintf(item->title, sizeof(item->title), "%s", title ? title : "");
    snprintf(item->path, sizeof(item->path), "%s", path ? path : "");
    item->size_bytes = size_bytes;
    snprintf(item->size_str, sizeof(item->size_str), "%s", size_str ? size_str : "");
    item->has_thumbnail = (has_thumbnail != 0);
    item->year = year;
    snprintf(item->director, sizeof(item->director), "%s", director ? director : "");
    snprintf(item->cast, sizeof(item->cast), "%s", cast ? cast : "");
    snprintf(item->genres, sizeof(item->genres), "%s", genres ? genres : "");
    snprintf(item->overview, sizeof(item->overview), "%s", overview ? overview : "");
}

void tdvd_desktop_sync_catalog_end(uint64_t generation) {
    if (!g_desktop_lib_inited) return;
    if (g_desktop_lib.catalog_count > 0) {
        snprintf(g_desktop_lib.default_disc_path, sizeof(g_desktop_lib.default_disc_path),
                 "%s", g_desktop_lib.catalog[0].path);
        snprintf(g_desktop_lib.default_disc_id, sizeof(g_desktop_lib.default_disc_id),
                 "%s", g_desktop_lib.catalog[0].id);
        snprintf(g_desktop_lib.default_disc_title, sizeof(g_desktop_lib.default_disc_title),
                 "%s", g_desktop_lib.catalog[0].title);
    } else {
        g_desktop_lib.default_disc_path[0] = '\0';
        g_desktop_lib.default_disc_id[0] = '\0';
        g_desktop_lib.default_disc_title[0] = '\0';
    }
    g_desktop_lib.catalog_generation = generation;
    pthread_cond_broadcast(&g_desktop_lib.catalog_cond);
    pthread_mutex_unlock(&g_desktop_lib.catalog_lock);
}

void tdvd_desktop_sync_import_status(
    int state, int active, int64_t start_time,
    const char *stage_msg, const char *error_msg,
    const char *result_disc_id, const char *result_disc_path,
    const char *result_title, const char *logs_joined
) {
    ensure_desktop_lib_inited();
    import_job_t *job = &g_desktop_lib.import_job;
    pthread_mutex_lock(&job->lock);
    job->state = (import_state_t)state;
    job->active = (active != 0);
    job->start_time = (time_t)start_time;
    snprintf(job->stage_msg, sizeof(job->stage_msg), "%s", stage_msg ? stage_msg : "");
    snprintf(job->error_msg, sizeof(job->error_msg), "%s", error_msg ? error_msg : "");
    snprintf(job->result_disc_id, sizeof(job->result_disc_id), "%s", result_disc_id ? result_disc_id : "");
    snprintf(job->result_disc_path, sizeof(job->result_disc_path), "%s", result_disc_path ? result_disc_path : "");
    snprintf(job->result_title, sizeof(job->result_title), "%s", result_title ? result_title : "");

    job->log_count = 0;
    if (logs_joined && logs_joined[0]) {
        const char *p = logs_joined;
        while (*p && job->log_count < IMPORT_MAX_LOGS) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            if (len >= IMPORT_LOG_LINE_LEN) len = IMPORT_LOG_LINE_LEN - 1;
            memcpy(job->log_lines[job->log_count], p, len);
            job->log_lines[job->log_count][len] = '\0';
            job->log_count++;
            if (!nl) break;
            p = nl + 1;
        }
    }
    pthread_mutex_unlock(&job->lock);
}

void tdvd_library_sanitize_id(const char *in, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    if (!in) {
        out[0] = '\0';
        return;
    }
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < out_len; i++) {
        char c = in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') {
            out[j++] = c;
        } else {
            out[j++] = '_';
        }
    }
    out[j] = '\0';
}

bool tdvd_library_is_tdvd_file(const char *path) {
    if (!path) return false;
    size_t len = strlen(path);
    return (len > 5 && strcasecmp(path + len - 5, ".tdvd") == 0) ||
           (len > 8 && strcasecmp(path + len - 8, ".tinydvd") == 0);
}

bool tdvd_library_is_dvd_dir(const char *dir_path) {
    if (!dir_path || !dir_path[0]) return false;
    struct stat st;
    char probe[1536];
    snprintf(probe, sizeof(probe), "%s/VIDEO_TS.IFO", dir_path);
    if (stat(probe, &st) == 0) return true;
    snprintf(probe, sizeof(probe), "%s/VIDEO_TS/VIDEO_TS.IFO", dir_path);
    if (stat(probe, &st) == 0) return true;
    return false;
}

void tdvd_library_init(tdvd_library_t *lib, const char *target_path) {
    (void)lib;
    (void)target_path;
    ensure_desktop_lib_inited();
}

void tdvd_library_destroy(tdvd_library_t *lib) {
    if (!lib) return;
    pthread_mutex_lock(&lib->catalog_lock);
    free(lib->catalog);
    lib->catalog = NULL;
    lib->catalog_count = 0;
    lib->catalog_cap = 0;
    pthread_mutex_unlock(&lib->catalog_lock);
}

void tdvd_library_rescan(tdvd_library_t *lib) {
    if (!lib) return;
    pthread_mutex_lock(&lib->catalog_lock);
    bool is_lib = lib->is_library;
    char path_copy[1024];
    snprintf(path_copy, sizeof(path_copy), "%s", lib->target_path);
    pthread_mutex_unlock(&lib->catalog_lock);
    if (g_cb_set_dir && is_lib && path_copy[0]) {
        g_cb_set_dir(path_copy);
    }
}

bool tdvd_library_check_for_changes(tdvd_library_t *lib) {
    (void)lib;
    return false;
}

uint64_t tdvd_library_get_generation(tdvd_library_t *lib) {
    if (!lib) return 0;
    pthread_mutex_lock(&lib->catalog_lock);
    uint64_t gen = lib->catalog_generation;
    pthread_mutex_unlock(&lib->catalog_lock);
    return gen;
}

void tdvd_library_wake_watchers(tdvd_library_t *lib) {
    if (!lib) return;
    pthread_mutex_lock(&lib->catalog_lock);
    pthread_cond_broadcast(&lib->catalog_cond);
    pthread_mutex_unlock(&lib->catalog_lock);
}

void tdvd_library_set_directory(tdvd_library_t *lib, const char *dir_path) {
    if (!lib || !dir_path || !dir_path[0]) return;
    if (g_cb_set_dir) {
        g_cb_set_dir(dir_path);
    }
}

uint8_t *tdvd_library_read_thumbnail(const char *disc_path, size_t *out_size) {
    if (out_size) *out_size = 0;
    if (!disc_path || !disc_path[0] || !g_cb_read_thumb) return NULL;
    const uint8_t *py_ptr = NULL;
    size_t py_sz = 0;
    if (!g_cb_read_thumb(disc_path, &py_ptr, &py_sz) || !py_ptr || py_sz == 0 || py_sz > 32 * 1024 * 1024) {
        return NULL;
    }
    uint8_t *copy = (uint8_t *)malloc(py_sz);
    if (!copy) return NULL;
    memcpy(copy, py_ptr, py_sz);
    if (out_size) *out_size = py_sz;
    return copy;
}

void tdvd_library_detect_default_source(char *out_source, size_t source_len,
                                        char *out_label, size_t label_len) {
    if (out_source && source_len > 0) out_source[0] = '\0';
    if (out_label && label_len > 0) out_label[0] = '\0';
    if (g_cb_detect_source) {
        g_cb_detect_source(out_source, source_len, out_label, label_len);
    }
}

bool tdvd_library_import_start(tdvd_library_t *lib, const char *source, const char *title,
                               bool skip_enrichment, char *err_buf, size_t err_len) {
    (void)lib;
    if (err_buf && err_len > 0) err_buf[0] = '\0';
    if (!g_cb_import_start) {
        if (err_buf && err_len > 0) snprintf(err_buf, err_len, "Import callback not initialized.");
        return false;
    }
    return g_cb_import_start(source ? source : "", title ? title : "",
                             skip_enrichment ? 1 : 0, err_buf, err_len) != 0;
}

void tdvd_library_import_cancel(tdvd_library_t *lib) {
    (void)lib;
    if (g_cb_import_cancel) {
        g_cb_import_cancel();
    }
}

const char *tdvd_import_state_name(import_state_t st) {
    switch (st) {
        case IMPORT_RIPPING:    return "ripping";
        case IMPORT_CONVERTING: return "converting";
        case IMPORT_DONE:       return "done";
        case IMPORT_ERROR:      return "error";
        default:                return "idle";
    }
}

void tdvd_free(void *ptr) {
    free(ptr);
}

/* =========================================================================
 * Part B: Desktop SDL2 / OpenGL 2.1 / mpv Player Runner
 * ========================================================================= */

static volatile sig_atomic_t g_quit_requested = 0;

static void handle_sigint(int sig) {
    (void)sig;
    g_quit_requested = 1;
}

static void *get_proc_address_sdl(void *ctx, const char *name) {
    (void)ctx;
    return SDL_GL_GetProcAddress(name);
}

static bool window_to_dvd(SDL_Window *window, float display_aspect, int frame_height,
                          int x, int y, int *x_dvd, int *y_dvd) {
    int win_w, win_h;
    SDL_GetWindowSize(window, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0) return false;
    float win_aspect = (float)win_w / (float)win_h;
    float vp_x = 0.0f, vp_y = 0.0f, vp_w = (float)win_w, vp_h = (float)win_h;
    if (win_aspect > display_aspect) {
        vp_w = (float)win_h * display_aspect;
        vp_x = ((float)win_w - vp_w) * 0.5f;
    } else if (win_aspect < display_aspect) {
        vp_h = (float)win_w / display_aspect;
        vp_y = ((float)win_h - vp_h) * 0.5f;
    }
    *x_dvd = (int)(((x - vp_x) * 720.0f) / vp_w);
    *y_dvd = (int)(((y - vp_y) * (float)frame_height) / vp_h);
    return true;
}

static void on_mpv_update_cb(void *user_data) {
    (void)user_data;
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = WAKEUP_EVENT;
    SDL_PushEvent(&ev);
}

static bool start_movie_playback(SDL_Window *window, mpv_controller_t *mpv,
                                 spu_compositor_t **spu, dvd_vm_bridge_t **bridge,
                                 const char *disc_path, const char *disc_title,
                                 char *current_title, size_t title_len,
                                 bool *in_player_view, native_ui_t *ui) {
    if (*bridge) {
        dvd_vm_bridge_destroy(*bridge);
        *bridge = NULL;
    }
    mpv_handle *mh = mpv_controller_get_handle(mpv);
    if (mh) mpv_command_string(mh, "stop");
    player_adapter_reset(mpv_controller_get_adapter(mpv));

    if (*spu) {
        spu_compositor_destroy(*spu);
    }
    *spu = spu_compositor_create();
    if (!*spu) return false;

    dvd_vm_bridge_t *new_bridge = dvd_vm_bridge_init(disc_path, mpv_controller_get_adapter(mpv), *spu);
    if (!new_bridge) {
        fprintf(stderr, "Failed to initialize DVD VM bridge for '%s'\n", disc_path);
        return false;
    }
    *bridge = new_bridge;
    dvd_vm_bridge_start(*bridge);
    *in_player_view = true;

    snprintf(current_title, title_len, "%s", (disc_title && disc_title[0]) ? disc_title : disc_path);
    if (window) {
        char wtitle[320];
        snprintf(wtitle, sizeof(wtitle), "TinyDVD — %s", current_title);
        SDL_SetWindowTitle(window, wtitle);
    }
    if (ui) native_ui_mark_dirty(ui);
    return true;
}

static void return_to_library(SDL_Window *window, mpv_controller_t *mpv,
                              spu_compositor_t **spu, dvd_vm_bridge_t **bridge,
                              tdvd_library_t *lib, const char *fallback_dir,
                              bool *in_player_view, bool *cursor_visible,
                              native_ui_t *ui) {
    if (*bridge) {
        dvd_vm_bridge_destroy(*bridge);
        *bridge = NULL;
    }
    mpv_handle *mh = mpv_controller_get_handle(mpv);
    if (mh) mpv_command_string(mh, "stop");
    player_adapter_reset(mpv_controller_get_adapter(mpv));

    if (*spu) {
        spu_compositor_destroy(*spu);
        *spu = spu_compositor_create();
    }

    if (!lib->is_library && fallback_dir && fallback_dir[0]) {
        tdvd_library_set_directory(lib, fallback_dir);
    } else {
        tdvd_library_rescan(lib);
    }

    SDL_ShowCursor(SDL_ENABLE);
    *cursor_visible = true;
    *in_player_view = false;
    if (window) {
        SDL_SetWindowTitle(window, "TinyDVD Cinema");
    }
    if (ui) native_ui_mark_dirty(ui);
}

int tdvd_desktop_run(const char *initial_dvd_path, int start_fullscreen) {
    ensure_desktop_lib_inited();
    tdvd_library_t *lib = &g_desktop_lib;
    g_quit_requested = 0;

    char library_fallback_dir[1024] = {0};
    if (!lib->is_library) {
        snprintf(library_fallback_dir, sizeof(library_fallback_dir), "%s", lib->target_path);
        char *sl = strrchr(library_fallback_dir, '/');
        if (sl && sl > library_fallback_dir) *sl = '\0';
        else if (sl == library_fallback_dir) library_fallback_dir[1] = '\0';
        else snprintf(library_fallback_dir, sizeof(library_fallback_dir), ".");
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        tdvd_library_destroy(lib);
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    Uint32 window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN |
                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (start_fullscreen) {
        window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

    int init_w = lib->is_library ? 1280 : 853;
    int init_h = lib->is_library ? 800 : 480;
    const char *init_title = lib->is_library ? "TinyDVD Cinema" : "tinydvd Player";

    SDL_Window *window = SDL_CreateWindow(
        init_title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        init_w, init_h,
        window_flags
    );
    if (!window) {
        fprintf(stderr, "Failed to create SDL window: %s\n", SDL_GetError());
        tdvd_library_destroy(lib);
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) {
        fprintf(stderr, "Failed to create OpenGL context: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        tdvd_library_destroy(lib);
        SDL_Quit();
        return 1;
    }

    SDL_GL_MakeCurrent(window, gl_ctx);
    SDL_GL_SetSwapInterval(1);
    SDL_StartTextInput();
    SDL_DisableScreenSaver();

    mpv_controller_t *mpv = mpv_controller_init(get_proc_address_sdl, on_mpv_update_cb, NULL);
    if (!mpv) {
        fprintf(stderr, "Failed to initialize mpv_controller\n");
        SDL_StopTextInput();
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        tdvd_library_destroy(lib);
        SDL_Quit();
        return 1;
    }

    spu_compositor_t *spu = spu_compositor_create();
    if (!spu) {
        fprintf(stderr, "Failed to initialize spu_compositor\n");
        mpv_controller_destroy(mpv);
        SDL_StopTextInput();
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        tdvd_library_destroy(lib);
        SDL_Quit();
        return 1;
    }

    native_ui_t *ui = native_ui_create(window, lib);
    dvd_vm_bridge_t *bridge = NULL;
    bool in_player_view = false;
    char current_movie_title[256] = {0};

    if (!lib->is_library) {
        const char *play_target = (lib->catalog_count > 0) ? lib->catalog[0].path : lib->target_path;
        const char *play_title = (lib->catalog_count > 0) ? lib->catalog[0].title : (initial_dvd_path ? initial_dvd_path : "");
        if (!start_movie_playback(window, mpv, &spu, &bridge, play_target, play_title,
                                  current_movie_title, sizeof(current_movie_title),
                                  &in_player_view, ui)) {
            if (ui) native_ui_destroy(ui);
            if (spu) spu_compositor_destroy(spu);
            mpv_controller_destroy(mpv);
            SDL_StopTextInput();
            SDL_GL_DeleteContext(gl_ctx);
            SDL_DestroyWindow(window);
            tdvd_library_destroy(lib);
            SDL_Quit();
            return 1;
        }
    }

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    bool running = true;
    bool is_fullscreen = (start_fullscreen != 0);
    bool cursor_visible = true;
    Uint32 last_mouse_activity = SDL_GetTicks();

    while (running && !g_quit_requested) {
        if (in_player_view && bridge && !dvd_vm_bridge_is_running(bridge)) {
            return_to_library(window, mpv, &spu, &bridge, lib, library_fallback_dir,
                              &in_player_view, &cursor_visible, ui);
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_MOUSEMOTION || ev.type == SDL_MOUSEBUTTONDOWN ||
                ev.type == SDL_MOUSEBUTTONUP || ev.type == SDL_MOUSEWHEEL) {
                last_mouse_activity = SDL_GetTicks();
                if (!cursor_visible) {
                    SDL_ShowCursor(SDL_ENABLE);
                    cursor_visible = true;
                    if (ui) native_ui_mark_dirty(ui);
                }
            }

            if (ev.type == SDL_QUIT) {
                running = false;
                break;
            }

            char play_path[1024] = {0};
            char play_title[256] = {0};
            bool back_to_lib = false;
            if (ui && native_ui_handle_event(ui, &ev, in_player_view,
                                             play_path, sizeof(play_path),
                                             play_title, sizeof(play_title),
                                             &back_to_lib)) {
                if (play_path[0]) {
                    start_movie_playback(window, mpv, &spu, &bridge, play_path, play_title,
                                         current_movie_title, sizeof(current_movie_title),
                                         &in_player_view, ui);
                    last_mouse_activity = SDL_GetTicks();
                } else if (back_to_lib && in_player_view) {
                    return_to_library(window, mpv, &spu, &bridge, lib, library_fallback_dir,
                                      &in_player_view, &cursor_visible, ui);
                }
                continue;
            }

            switch (ev.type) {
                case SDL_KEYDOWN:
                    switch (ev.key.keysym.sym) {
                        case SDLK_q:
                            running = false;
                            break;
                        case SDLK_UP:
                            if (in_player_view && bridge) dvd_vm_bridge_up_button(bridge);
                            break;
                        case SDLK_DOWN:
                            if (in_player_view && bridge) dvd_vm_bridge_down_button(bridge);
                            break;
                        case SDLK_LEFT:
                            if (in_player_view && bridge) {
                                if (ev.key.keysym.mod & KMOD_SHIFT) {
                                    dvd_vm_bridge_seek_relative(bridge, -60);
                                } else {
                                    dvd_vm_bridge_left_button(bridge);
                                }
                            }
                            break;
                        case SDLK_RIGHT:
                            if (in_player_view && bridge) {
                                if (ev.key.keysym.mod & KMOD_SHIFT) {
                                    dvd_vm_bridge_seek_relative(bridge, +60);
                                } else {
                                    dvd_vm_bridge_right_button(bridge);
                                }
                            }
                            break;
                        case SDLK_COMMA:
                            if (in_player_view && bridge) dvd_vm_bridge_seek_relative(bridge, -10);
                            break;
                        case SDLK_PERIOD:
                            if (in_player_view && bridge) dvd_vm_bridge_seek_relative(bridge, +10);
                            break;
                        case SDLK_LEFTBRACKET:
                        case SDLK_PAGEUP:
                            if (in_player_view && bridge) dvd_vm_bridge_prev_chapter(bridge);
                            break;
                        case SDLK_RIGHTBRACKET:
                        case SDLK_PAGEDOWN:
                            if (in_player_view && bridge) dvd_vm_bridge_next_chapter(bridge);
                            break;
                        case SDLK_0:
                        case SDLK_1:
                        case SDLK_2:
                        case SDLK_3:
                        case SDLK_4:
                        case SDLK_5:
                        case SDLK_6:
                        case SDLK_7:
                        case SDLK_8:
                        case SDLK_9: {
                            if (in_player_view && bridge) {
                                int digit = (int)(ev.key.keysym.sym - SDLK_0);
                                dvd_vm_bridge_seek_fraction(bridge, (double)digit * 0.10);
                            }
                            break;
                        }
                        case SDLK_c:
                        case SDLK_s:
                            if (in_player_view && bridge) dvd_vm_bridge_cycle_subtitles(bridge);
                            break;
                        case SDLK_a:
                            if (in_player_view && bridge) dvd_vm_bridge_cycle_audio(bridge);
                            break;
                        case SDLK_RETURN:
                        case SDLK_KP_ENTER:
                            if (in_player_view && bridge) dvd_vm_bridge_activate_button(bridge);
                            break;
                        case SDLK_SPACE:
                            if (in_player_view && bridge) dvd_vm_bridge_toggle_pause(bridge);
                            break;
                        case SDLK_m:
                            if (in_player_view && bridge) dvd_vm_bridge_menu(bridge);
                            break;
                        case SDLK_ESCAPE:
                            if (in_player_view && bridge) {
                                if (!dvd_vm_bridge_escape(bridge)) {
                                    return_to_library(window, mpv, &spu, &bridge, lib, library_fallback_dir,
                                                      &in_player_view, &cursor_visible, ui);
                                }
                            } else {
                                running = false;
                            }
                            break;
                        case SDLK_f:
                            is_fullscreen = !is_fullscreen;
                            SDL_SetWindowFullscreen(window, is_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                            if (ui) native_ui_mark_dirty(ui);
                            break;
                        default:
                            break;
                    }
                    break;

                case SDL_MOUSEMOTION:
                    if (in_player_view && bridge && spu) {
                        int x_dvd, y_dvd;
                        if (window_to_dvd(window, mpv_controller_get_display_aspect(mpv),
                                          spu_compositor_get_frame_height(spu),
                                          ev.motion.x, ev.motion.y, &x_dvd, &y_dvd)) {
                            dvd_vm_bridge_mouse_move(bridge, x_dvd, y_dvd);
                        }
                    }
                    break;

                case SDL_MOUSEBUTTONDOWN:
                    if (in_player_view && bridge && spu) {
                        int x_dvd, y_dvd;
                        if (ev.button.button == SDL_BUTTON_LEFT &&
                            window_to_dvd(window, mpv_controller_get_display_aspect(mpv),
                                          spu_compositor_get_frame_height(spu),
                                          ev.button.x, ev.button.y, &x_dvd, &y_dvd)) {
                            dvd_vm_bridge_mouse_click(bridge, x_dvd, y_dvd);
                        }
                    }
                    break;

                default:
                    break;
            }
        }

        if (in_player_view && !(ui && native_ui_is_modal_open(ui))) {
            if (cursor_visible && SDL_GetTicks() - last_mouse_activity >= CURSOR_IDLE_MS) {
                SDL_ShowCursor(SDL_DISABLE);
                cursor_visible = false;
                if (ui) native_ui_mark_dirty(ui);
            }
        } else if (!cursor_visible) {
            SDL_ShowCursor(SDL_ENABLE);
            cursor_visible = true;
            if (ui) native_ui_mark_dirty(ui);
        }

        mpv_controller_poll_events(mpv);

        int draw_w, draw_h;
        SDL_GL_GetDrawableSize(window, &draw_w, &draw_h);

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (in_player_view) {
            mpv_controller_render(mpv, draw_w, draw_h);
            spu_compositor_render_gl(spu, draw_w, draw_h, mpv_controller_get_display_aspect(mpv));
            if (ui) {
                bool hud_vis = cursor_visible || native_ui_is_modal_open(ui);
                native_ui_render(ui, draw_w, draw_h, true, current_movie_title, hud_vis);
            }
        } else if (ui) {
            native_ui_render(ui, draw_w, draw_h, false, NULL, false);
        }

        SDL_GL_SwapWindow(window);
        SDL_Delay(10);
    }

    printf("[main] Cleaning up...\n");
    SDL_StopTextInput();
    SDL_EnableScreenSaver();
    if (ui) native_ui_destroy(ui);
    if (bridge) dvd_vm_bridge_destroy(bridge);
    if (spu) spu_compositor_destroy(spu);
    mpv_controller_destroy(mpv);
    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    tdvd_library_destroy(lib);
    SDL_Quit();
    return 0;
}

/* =========================================================================
 * Part C: Web / Headless DVD VM Session Bridge
 * ========================================================================= */

typedef void (*tdvd_vm_json_cb_t)(void *user_data, const char *json_str);

typedef struct tdvd_vm_session {
    dvd_vm_bridge_t *vm;
    spu_compositor_t *spu;
    player_adapter_t adapter;
    pthread_mutex_t lock;
    pthread_mutex_t state_lock;

    char disc_id[128];
    tdvd_vm_json_cb_t cb;
    void *user_data;

    char *last_highlight_url;
    int last_frame_w;
    int last_frame_h;
    int last_active_btn;
    int last_button_count;
    dvd_vm_button_rect_t last_buttons[36];
} tdvd_vm_session_t;

static void escape_json_str(const char *in, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    if (!in) {
        out[0] = '\0';
        return;
    }
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

static void emit_session_json(tdvd_vm_session_t *s, const char *json) {
    if (s && s->cb && json) {
        s->cb(s->user_data, json);
    }
}

static int sess_adapter_load_file(void *ctx, const char *video_path, const char *subfile_name,
                                  const char *audio_path, uint64_t seq) {
    (void)audio_path;
    tdvd_vm_session_t *s = (tdvd_vm_session_t *)ctx;
    if (!s) return -1;

    char base[64] = {0};
    const char *target_name = (subfile_name && subfile_name[0]) ? subfile_name : video_path;
    if (target_name) {
        const char *slash = strrchr(target_name, '/');
        const char *fn = slash ? slash + 1 : target_name;
        snprintf(base, sizeof(base), "%s", fn);
        char *dot = strrchr(base, '.');
        if (dot) *dot = '\0';
    }

    char e_base[128] = {0}, e_disc[256] = {0};
    escape_json_str(base, e_base, sizeof(e_base));

    pthread_mutex_lock(&s->state_lock);
    escape_json_str(s->disc_id, e_disc, sizeof(e_disc));
    if (s->last_highlight_url) {
        free(s->last_highlight_url);
        s->last_highlight_url = NULL;
    }
    s->last_button_count = 0;
    s->last_active_btn = 0;
    pthread_mutex_unlock(&s->state_lock);

    char buf[1024];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"video_load\",\"src\":\"/api/media/%s.mp4?disc=%s\",\"pts\":0.000,\"paused\":false,\"seq\":%llu}",
             e_base, e_disc, (unsigned long long)seq);
    emit_session_json(s, buf);
    emit_session_json(s, "{\"type\":\"highlight\",\"data_url\":null,\"w\":720,\"h\":480,\"active_btn\":0,\"buttons\":[]}");
    return 0;
}

static void sess_adapter_seek_exact(void *ctx, uint64_t pts_nanos, bool paused, uint64_t seq) {
    tdvd_vm_session_t *s = (tdvd_vm_session_t *)ctx;
    if (!s) return;
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"seek\",\"pts\":%.3f,\"paused\":%s,\"seq\":%llu}",
             (double)pts_nanos / 1e9, paused ? "true" : "false", (unsigned long long)seq);
    emit_session_json(s, buf);
}

static void sess_adapter_set_paused(void *ctx, bool paused) {
    tdvd_vm_session_t *s = (tdvd_vm_session_t *)ctx;
    if (!s) return;
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"type\":\"pause\",\"paused\":%s}", paused ? "true" : "false");
    emit_session_json(s, buf);
}

static void sess_adapter_set_audio_track(void *ctx, int aid) {
    (void)ctx;
    (void)aid;
}

static void sess_adapter_set_subtitle_track(void *ctx, int sid) {
    (void)ctx;
    (void)sid;
}

static void sess_adapter_osd_message(void *ctx, const char *msg) {
    tdvd_vm_session_t *s = (tdvd_vm_session_t *)ctx;
    if (!s || !msg) return;
    char e_msg[512] = {0};
    escape_json_str(msg, e_msg, sizeof(e_msg));
    char buf[640];
    snprintf(buf, sizeof(buf), "{\"type\":\"osd\",\"text\":\"%s\"}", e_msg);
    emit_session_json(s, buf);
}

static void sess_adapter_on_highlight(void *ctx, const dvd_vm_highlight_info_t *hl) {
    tdvd_vm_session_t *s = (tdvd_vm_session_t *)ctx;
    if (!s || !hl) return;

    pthread_mutex_lock(&s->state_lock);
    bool same_url = (!hl->highlight_data_url && !s->last_highlight_url) ||
                    (hl->highlight_data_url && s->last_highlight_url &&
                     strcmp(hl->highlight_data_url, s->last_highlight_url) == 0);
    int bcnt = hl->button_count > 36 ? 36 : hl->button_count;
    bool unchanged = same_url &&
                     s->last_frame_w == hl->frame_w && s->last_frame_h == hl->frame_h &&
                     s->last_active_btn == hl->active_button &&
                     s->last_button_count == hl->button_count &&
                     memcmp(s->last_buttons, hl->buttons, sizeof(hl->buttons[0]) * (size_t)bcnt) == 0;
    pthread_mutex_unlock(&s->state_lock);
    if (unchanged) return;

    char *hl_url_copy = hl->highlight_data_url ? strdup(hl->highlight_data_url) : NULL;

    pthread_mutex_lock(&s->state_lock);
    free(s->last_highlight_url);
    s->last_highlight_url = hl_url_copy;
    s->last_frame_w = hl->frame_w;
    s->last_frame_h = hl->frame_h;
    s->last_active_btn = hl->active_button;
    s->last_button_count = hl->button_count;
    for (int i = 0; i < bcnt; i++) {
        s->last_buttons[i] = hl->buttons[i];
    }
    pthread_mutex_unlock(&s->state_lock);

    char btn_json[4096] = "[";
    size_t bpos = 1;
    for (int i = 0; i < bcnt; i++) {
        char item[128];
        snprintf(item, sizeof(item), "%s{\"btn\":%d,\"x1\":%d,\"y1\":%d,\"x2\":%d,\"y2\":%d}",
                 (i > 0) ? "," : "",
                 hl->buttons[i].btn_id,
                 hl->buttons[i].x1, hl->buttons[i].y1,
                 hl->buttons[i].x2, hl->buttons[i].y2);
        size_t ilen = strlen(item);
        if (bpos + ilen + 2 < sizeof(btn_json)) {
            memcpy(btn_json + bpos, item, ilen);
            bpos += ilen;
            btn_json[bpos] = '\0';
        }
    }
    btn_json[bpos++] = ']';
    btn_json[bpos] = '\0';

    if (hl->highlight_data_url) {
        size_t dlen = strlen(hl->highlight_data_url);
        size_t msg_len = dlen + strlen(btn_json) + 256;
        char *dyn = (char *)malloc(msg_len);
        if (dyn) {
            snprintf(dyn, msg_len,
                     "{\"type\":\"highlight\",\"data_url\":\"%s\",\"w\":%d,\"h\":%d,\"active_btn\":%d,\"buttons\":%s}",
                     hl->highlight_data_url, hl->frame_w, hl->frame_h, hl->active_button, btn_json);
            emit_session_json(s, dyn);
            free(dyn);
        }
    } else {
        size_t msg_len = strlen(btn_json) + 256;
        char *dyn = (char *)malloc(msg_len);
        if (dyn) {
            snprintf(dyn, msg_len,
                     "{\"type\":\"highlight\",\"data_url\":null,\"w\":%d,\"h\":%d,\"active_btn\":%d,\"buttons\":%s}",
                     hl->frame_w, hl->frame_h, hl->active_button, btn_json);
            emit_session_json(s, dyn);
            free(dyn);
        }
    }
}

static void sess_adapter_on_state(void *ctx, int domain, int vts_nr, int pgc_nr, int cell_nr, int chapter_nr) {
    tdvd_vm_session_t *s = (tdvd_vm_session_t *)ctx;
    if (!s) return;
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"domain_state\",\"domain\":%d,\"vts\":%d,\"pgc\":%d,\"cell\":%d,\"chapter\":%d}",
             domain, vts_nr, pgc_nr, cell_nr, chapter_nr);
    emit_session_json(s, buf);
}

static const player_adapter_ops_t s_session_adapter_ops = {
    .load_file = sess_adapter_load_file,
    .seek_exact = sess_adapter_seek_exact,
    .set_paused = sess_adapter_set_paused,
    .set_audio_track = sess_adapter_set_audio_track,
    .set_subtitle_track = sess_adapter_set_subtitle_track,
    .osd_message = sess_adapter_osd_message,
    .on_highlight = sess_adapter_on_highlight,
    .on_state = sess_adapter_on_state,
};

tdvd_vm_session_t *tdvd_vm_session_create(const char *disc_path, const char *disc_id,
                                          tdvd_vm_json_cb_t cb, void *user_data) {
    if (!disc_path || !disc_path[0]) return NULL;
    tdvd_vm_session_t *s = (tdvd_vm_session_t *)calloc(1, sizeof(tdvd_vm_session_t));
    if (!s) return NULL;

    pthread_mutex_init(&s->lock, NULL);
    pthread_mutex_init(&s->state_lock, NULL);
    snprintf(s->disc_id, sizeof(s->disc_id), "%s", disc_id ? disc_id : "");
    s->cb = cb;
    s->user_data = user_data;
    s->last_frame_w = 720;
    s->last_frame_h = 480;

    player_adapter_init(&s->adapter, &s_session_adapter_ops, s);
    s->spu = spu_compositor_create();
    s->vm = dvd_vm_bridge_init(disc_path, &s->adapter, s->spu);
    if (!s->vm) {
        if (s->spu) spu_compositor_destroy(s->spu);
        player_adapter_destroy(&s->adapter);
        pthread_mutex_destroy(&s->state_lock);
        pthread_mutex_destroy(&s->lock);
        free(s);
        return NULL;
    }
    dvd_vm_bridge_start(s->vm);
    return s;
}

void tdvd_vm_session_destroy(tdvd_vm_session_t *s) {
    if (!s) return;

    pthread_mutex_lock(&s->lock);
    dvd_vm_bridge_t *vm = s->vm;
    s->vm = NULL;
    spu_compositor_t *spu = s->spu;
    s->spu = NULL;
    pthread_mutex_unlock(&s->lock);

    if (vm) dvd_vm_bridge_destroy(vm);
    if (spu) spu_compositor_destroy(spu);
    player_adapter_destroy(&s->adapter);

    pthread_mutex_lock(&s->state_lock);
    free(s->last_highlight_url);
    s->last_highlight_url = NULL;
    pthread_mutex_unlock(&s->state_lock);

    pthread_mutex_destroy(&s->state_lock);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

int tdvd_vm_session_load(tdvd_vm_session_t *s, const char *disc_path, const char *disc_id) {
    if (!s || !disc_path || !disc_path[0]) return -1;

    pthread_mutex_lock(&s->lock);
    if (s->vm) {
        dvd_vm_bridge_destroy(s->vm);
        s->vm = NULL;
    }
    player_adapter_reset(&s->adapter);
    if (s->spu) {
        spu_compositor_destroy(s->spu);
    }
    s->spu = spu_compositor_create();

    pthread_mutex_lock(&s->state_lock);
    snprintf(s->disc_id, sizeof(s->disc_id), "%s", disc_id ? disc_id : "");
    free(s->last_highlight_url);
    s->last_highlight_url = NULL;
    s->last_button_count = 0;
    s->last_active_btn = 0;
    pthread_mutex_unlock(&s->state_lock);

    s->vm = dvd_vm_bridge_init(disc_path, &s->adapter, s->spu);
    int rc = -1;
    if (s->vm) {
        dvd_vm_bridge_start(s->vm);
        rc = 0;
    }
    pthread_mutex_unlock(&s->lock);
    return rc;
}

void tdvd_vm_session_dispatch_action(tdvd_vm_session_t *s, const char *action, int x, int y) {
    if (!s || !action) return;
    pthread_mutex_lock(&s->lock);
    dvd_vm_bridge_t *vm = s->vm;
    if (vm) {
        if (strcmp(action, "Up") == 0) dvd_vm_bridge_up_button(vm);
        else if (strcmp(action, "Down") == 0) dvd_vm_bridge_down_button(vm);
        else if (strcmp(action, "Left") == 0) dvd_vm_bridge_left_button(vm);
        else if (strcmp(action, "Right") == 0) dvd_vm_bridge_right_button(vm);
        else if (strcmp(action, "Activate") == 0) dvd_vm_bridge_activate_button(vm);
        else if (strcmp(action, "Menu") == 0) dvd_vm_bridge_menu(vm);
        else if (strcmp(action, "Escape") == 0) dvd_vm_bridge_escape(vm);
        else if (strcmp(action, "TogglePause") == 0) dvd_vm_bridge_toggle_pause(vm);
        else if (strcmp(action, "PrevChapter") == 0) dvd_vm_bridge_prev_chapter(vm);
        else if (strcmp(action, "NextChapter") == 0) dvd_vm_bridge_next_chapter(vm);
        else if (strcmp(action, "CycleSubtitles") == 0) dvd_vm_bridge_cycle_subtitles(vm);
        else if (strcmp(action, "CycleAudio") == 0) dvd_vm_bridge_cycle_audio(vm);
        else if (strcmp(action, "mouse_move") == 0) dvd_vm_bridge_mouse_move(vm, x, y);
        else if (strcmp(action, "mouse_click") == 0) dvd_vm_bridge_mouse_click(vm, x, y);
    }
    pthread_mutex_unlock(&s->lock);
}

void tdvd_vm_session_set_paused(tdvd_vm_session_t *s, int paused) {
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    if (s->vm) {
        dvd_vm_bridge_set_paused(s->vm, paused != 0);
    }
    pthread_mutex_unlock(&s->lock);
}

void tdvd_vm_session_notify_playback(tdvd_vm_session_t *s, const char *event_type,
                                     uint64_t seq, int64_t pts_nanos, int paused) {
    if (!s || !event_type) return;
    player_adapter_t *a = &s->adapter;
    if (strcmp(event_type, "file_loaded") == 0) {
        player_adapter_notify_file_loaded(a, seq);
    } else if (strcmp(event_type, "seek_completed") == 0) {
        player_adapter_notify_seek_completed(a, seq, pts_nanos);
    } else if (strcmp(event_type, "time_update") == 0) {
        player_adapter_notify_time_pos(a, seq, pts_nanos);
    } else if (strcmp(event_type, "client_playback") == 0) {
        if (paused >= 0) {
            player_adapter_notify_paused(a, paused != 0);
        }
        if (pts_nanos >= 0) {
            player_adapter_notify_time_pos(a, seq, pts_nanos);
        }
    } else if (strcmp(event_type, "video_ended") == 0) {
        player_adapter_notify_ended(a, seq);
    }
}
