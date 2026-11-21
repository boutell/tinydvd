#include <SDL2/SDL.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __APPLE__
#include <OpenGL/gl.h>
extern bool native_dialog_choose_source(bool choose_iso_only, const char *initial_path,
                                        char *out_path, size_t out_len);
#else
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "mvob_parser.h"
#include "mpv_controller.h"
#include "spu_compositor.h"
#include "spu_png.h"
#include "dvd_vm_bridge.h"
#include "player_adapter.h"
#include "tdvd_archive.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define WAKEUP_EVENT (SDL_USEREVENT + 1)
#define MAX_PNG_CACHE 256

void tdvd_free(void *ptr) {
    free(ptr);
}

/* =========================================================================
 * Part A: Desktop Window, Player & Cairo/Pango/OpenGL Canvas Primitives
 * ========================================================================= */

typedef struct {
    int type;        /* 0=none, 1=quit, 2=mouse_motion, 3=mouse_down, 4=mouse_up, 5=mouse_wheel, 6=keydown, 7=textinput, 8=other,
                        9=file dropped or opened (fetch it with tdvd_desktop_take_dropped_file) */
    int x;
    int y;
    int button;
    int clicks;
    uint32_t motion_state;
    int wheel_y;
    int keysym;
    int scancode;
    uint16_t keymod;
    char text[64];
} tdvd_ui_event_t;

typedef struct {
    char key[128];
    cairo_surface_t *surface;
    double sar;
} tdvd_png_cache_entry_t;

typedef struct tdvd_desktop_ctx {
    SDL_Window *window;
    SDL_GLContext gl_ctx;
    mpv_controller_t *mpv;
    spu_compositor_t *spu;
    dvd_vm_bridge_t *bridge;

    GLuint gl_tex;
    bool gl_initialized;
    uint8_t *pixel_buf;
    int tex_w;
    int tex_h;

    cairo_surface_t *active_surf;
    cairo_t *active_cr;

    tdvd_png_cache_entry_t png_cache[MAX_PNG_CACHE];
    int png_cache_count;

    /* Last file dropped on the window, or opened from Finder (macOS sends
     * "open document" requests, which SDL reports as drops) */
    char *dropped_file;
} tdvd_desktop_ctx_t;

static volatile sig_atomic_t g_quit_requested = 0;

static void handle_sigint(int sig) {
    (void)sig;
    g_quit_requested = 1;
}

int tdvd_desktop_quit_requested(void) {
    return g_quit_requested ? 1 : 0;
}

static void *get_proc_address_sdl(void *ctx, const char *name) {
    (void)ctx;
    return SDL_GL_GetProcAddress(name);
}

static void on_mpv_update_cb(void *user_data) {
    (void)user_data;
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = WAKEUP_EVENT;
    SDL_PushEvent(&ev);
}

static bool window_to_dvd(SDL_Window *window, float display_aspect, int frame_height,
                          int x, int y, int *x_dvd, int *y_dvd) {
    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(window, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0 || frame_height <= 0 || display_aspect <= 0.0f) return false;
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

void tdvd_canvas_clear_png_cache(tdvd_desktop_ctx_t *ctx) {
    if (!ctx) return;
    for (int i = 0; i < ctx->png_cache_count; i++) {
        if (ctx->png_cache[i].surface) {
            cairo_surface_destroy(ctx->png_cache[i].surface);
            ctx->png_cache[i].surface = NULL;
        }
        ctx->png_cache[i].key[0] = '\0';
        ctx->png_cache[i].sar = 1.0;
    }
    ctx->png_cache_count = 0;
}

tdvd_desktop_ctx_t *tdvd_desktop_create(const char *title, int width, int height, int start_fullscreen) {
    g_quit_requested = 0;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return NULL;
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

    SDL_Window *window = SDL_CreateWindow(
        (title && title[0]) ? title : "TinyDVD Cinema",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width > 0 ? width : 1280,
        height > 0 ? height : 800,
        window_flags
    );
    if (!window) {
        fprintf(stderr, "Failed to create SDL window: %s\n", SDL_GetError());
        SDL_Quit();
        return NULL;
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) {
        fprintf(stderr, "Failed to create OpenGL context: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return NULL;
    }

    SDL_GL_MakeCurrent(window, gl_ctx);
    SDL_GL_SetSwapInterval(1);
    SDL_StartTextInput();
    SDL_DisableScreenSaver();

    mpv_controller_t *mpv = mpv_controller_init(get_proc_address_sdl, on_mpv_update_cb, NULL);
    if (!mpv) {
        fprintf(stderr, "Failed to initialize mpv_controller\n");
        SDL_StopTextInput();
        SDL_EnableScreenSaver();
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return NULL;
    }

    spu_compositor_t *spu = spu_compositor_create();
    if (!spu) {
        fprintf(stderr, "Failed to initialize spu_compositor\n");
        mpv_controller_destroy(mpv);
        SDL_StopTextInput();
        SDL_EnableScreenSaver();
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return NULL;
    }

    tdvd_desktop_ctx_t *ctx = (tdvd_desktop_ctx_t *)calloc(1, sizeof(tdvd_desktop_ctx_t));
    if (!ctx) {
        spu_compositor_destroy(spu);
        mpv_controller_destroy(mpv);
        SDL_StopTextInput();
        SDL_EnableScreenSaver();
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return NULL;
    }

    ctx->window = window;
    ctx->gl_ctx = gl_ctx;
    ctx->mpv = mpv;
    ctx->spu = spu;

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);
    return ctx;
}

void tdvd_desktop_destroy(tdvd_desktop_ctx_t *ctx) {
    if (!ctx) return;
    printf("[main] Cleaning up...\n");
    free(ctx->dropped_file);
    ctx->dropped_file = NULL;

    SDL_StopTextInput();
    SDL_EnableScreenSaver();

    if (ctx->active_cr) {
        cairo_destroy(ctx->active_cr);
        ctx->active_cr = NULL;
    }
    if (ctx->active_surf) {
        cairo_surface_destroy(ctx->active_surf);
        ctx->active_surf = NULL;
    }
    tdvd_canvas_clear_png_cache(ctx);

    if (ctx->gl_initialized && ctx->gl_tex) {
        glDeleteTextures(1, &ctx->gl_tex);
        ctx->gl_tex = 0;
        ctx->gl_initialized = false;
    }
    free(ctx->pixel_buf);
    ctx->pixel_buf = NULL;

    if (ctx->bridge) {
        dvd_vm_bridge_destroy(ctx->bridge);
        ctx->bridge = NULL;
    }
    if (ctx->spu) {
        spu_compositor_destroy(ctx->spu);
        ctx->spu = NULL;
    }
    if (ctx->mpv) {
        mpv_controller_destroy(ctx->mpv);
        ctx->mpv = NULL;
    }
    if (ctx->gl_ctx) {
        SDL_GL_DeleteContext(ctx->gl_ctx);
        ctx->gl_ctx = NULL;
    }
    if (ctx->window) {
        SDL_DestroyWindow(ctx->window);
        ctx->window = NULL;
    }
    SDL_Quit();
    free(ctx);
}

uint32_t tdvd_desktop_get_ticks(void) {
    return SDL_GetTicks();
}

void tdvd_desktop_get_sizes(tdvd_desktop_ctx_t *ctx, int *out_win_w, int *out_win_h,
                            int *out_draw_w, int *out_draw_h) {
    int ww = 0, wh = 0, dw = 0, dh = 0;
    if (ctx && ctx->window) {
        SDL_GetWindowSize(ctx->window, &ww, &wh);
        SDL_GL_GetDrawableSize(ctx->window, &dw, &dh);
    }
    if (out_win_w) *out_win_w = ww;
    if (out_win_h) *out_win_h = wh;
    if (out_draw_w) *out_draw_w = dw;
    if (out_draw_h) *out_draw_h = dh;
}

void tdvd_desktop_set_window_title(tdvd_desktop_ctx_t *ctx, const char *title) {
    if (ctx && ctx->window && title) {
        SDL_SetWindowTitle(ctx->window, title);
    }
}

void tdvd_desktop_set_fullscreen(tdvd_desktop_ctx_t *ctx, int fullscreen) {
    if (ctx && ctx->window) {
        SDL_SetWindowFullscreen(ctx->window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    }
}

void tdvd_desktop_set_cursor_visible(tdvd_desktop_ctx_t *ctx, int visible) {
    (void)ctx;
    SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE);
}

char *tdvd_desktop_get_clipboard(void) {
    char *sdl_txt = SDL_GetClipboardText();
    if (!sdl_txt) return NULL;
    char *dup = strdup(sdl_txt);
    SDL_free(sdl_txt);
    return dup;
}

void tdvd_desktop_set_clipboard(const char *text) {
    if (text) {
        SDL_SetClipboardText(text);
    }
}

int tdvd_desktop_choose_source_dialog(int choose_iso_only, const char *initial_path,
                                      char *out_path, size_t out_len) {
#ifdef __APPLE__
    return native_dialog_choose_source(choose_iso_only != 0, initial_path, out_path, out_len) ? 1 : 0;
#else
    (void)choose_iso_only;
    (void)initial_path;
    if (out_path && out_len > 0) out_path[0] = '\0';
    return 0;
#endif
}

int tdvd_desktop_take_dropped_file(tdvd_desktop_ctx_t *ctx, char *out_path, int out_len) {
    if (!ctx || !ctx->dropped_file || !out_path || out_len <= 0) return 0;
    snprintf(out_path, (size_t)out_len, "%s", ctx->dropped_file);
    free(ctx->dropped_file);
    ctx->dropped_file = NULL;
    return 1;
}

int tdvd_desktop_poll_event(tdvd_desktop_ctx_t *ctx, tdvd_ui_event_t *out_ev) {
    if (!out_ev) return 0;
    SDL_Event ev;
    if (!SDL_PollEvent(&ev)) {
        memset(out_ev, 0, sizeof(*out_ev));
        return 0;
    }

    memset(out_ev, 0, sizeof(*out_ev));
    switch (ev.type) {
        case SDL_QUIT:
            out_ev->type = 1;
            break;
        case SDL_MOUSEMOTION:
            out_ev->type = 2;
            out_ev->x = ev.motion.x;
            out_ev->y = ev.motion.y;
            out_ev->motion_state = ev.motion.state;
            break;
        case SDL_MOUSEBUTTONDOWN:
            out_ev->type = 3;
            out_ev->x = ev.button.x;
            out_ev->y = ev.button.y;
            out_ev->button = ev.button.button;
            out_ev->clicks = ev.button.clicks;
            out_ev->keymod = (uint16_t)SDL_GetModState();
            break;
        case SDL_MOUSEBUTTONUP:
            out_ev->type = 4;
            out_ev->x = ev.button.x;
            out_ev->y = ev.button.y;
            out_ev->button = ev.button.button;
            out_ev->clicks = ev.button.clicks;
            break;
        case SDL_MOUSEWHEEL:
            out_ev->type = 5;
            out_ev->wheel_y = ev.wheel.y;
            break;
        case SDL_KEYDOWN:
            out_ev->type = 6;
            out_ev->keysym = (int)ev.key.keysym.sym;
            out_ev->scancode = (int)ev.key.keysym.scancode;
            out_ev->keymod = (uint16_t)ev.key.keysym.mod;
            break;
        case SDL_TEXTINPUT:
            out_ev->type = 7;
            out_ev->keymod = (uint16_t)SDL_GetModState();
            snprintf(out_ev->text, sizeof(out_ev->text), "%s", ev.text.text);
            break;
        case SDL_DROPFILE:
        case SDL_DROPTEXT:
            out_ev->type = 8;
            if (ev.type == SDL_DROPFILE && ctx && ev.drop.file) {
                char *copy = strdup(ev.drop.file);
                if (copy) {
                    free(ctx->dropped_file);
                    ctx->dropped_file = copy;
                    out_ev->type = 9;
                }
            }
            SDL_free(ev.drop.file);
            break;
        default:
            out_ev->type = 8;
            break;
    }
    return 1;
}

int tdvd_desktop_start_movie(tdvd_desktop_ctx_t *ctx, const char *disc_path, const char *window_title) {
    if (!ctx || !disc_path || !disc_path[0]) return 0;

    if (ctx->bridge) {
        dvd_vm_bridge_destroy(ctx->bridge);
        ctx->bridge = NULL;
    }
    mpv_handle *mh = mpv_controller_get_handle(ctx->mpv);
    if (mh) mpv_command_string(mh, "stop");
    player_adapter_reset(mpv_controller_get_adapter(ctx->mpv));

    if (ctx->spu) {
        spu_compositor_destroy(ctx->spu);
    }
    ctx->spu = spu_compositor_create();
    if (!ctx->spu) return 0;

    dvd_vm_bridge_t *new_bridge = dvd_vm_bridge_init(disc_path, mpv_controller_get_adapter(ctx->mpv), ctx->spu);
    if (!new_bridge) {
        fprintf(stderr, "Failed to initialize DVD VM bridge for '%s'\n", disc_path);
        return 0;
    }
    ctx->bridge = new_bridge;
    dvd_vm_bridge_start(ctx->bridge);

    if (ctx->window && window_title && window_title[0]) {
        SDL_SetWindowTitle(ctx->window, window_title);
    }
    return 1;
}

void tdvd_desktop_stop_movie(tdvd_desktop_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->bridge) {
        dvd_vm_bridge_destroy(ctx->bridge);
        ctx->bridge = NULL;
    }
    mpv_handle *mh = ctx->mpv ? mpv_controller_get_handle(ctx->mpv) : NULL;
    if (mh) mpv_command_string(mh, "stop");
    if (ctx->mpv) {
        player_adapter_reset(mpv_controller_get_adapter(ctx->mpv));
    }
    if (ctx->spu) {
        spu_compositor_destroy(ctx->spu);
        ctx->spu = spu_compositor_create();
    }
}

int tdvd_desktop_is_vm_running(tdvd_desktop_ctx_t *ctx) {
    if (!ctx || !ctx->bridge) return 0;
    return dvd_vm_bridge_is_running(ctx->bridge) ? 1 : 0;
}

/* The one table of player actions, shared by the desktop player and web
 * sessions. Mouse coordinates are in DVD frame pixels. Returns 1 if the
 * action was recognised and taken. */
static int dispatch_vm_action(dvd_vm_bridge_t *vm, const char *action, int x, int y, double darg) {
    static const struct {
        const char *name;
        void (*fn)(dvd_vm_bridge_t *);
    } simple_actions[] = {
        { "up",           dvd_vm_bridge_up_button },
        { "down",         dvd_vm_bridge_down_button },
        { "left",         dvd_vm_bridge_left_button },
        { "right",        dvd_vm_bridge_right_button },
        { "activate",     dvd_vm_bridge_activate_button },
        { "pause",        dvd_vm_bridge_toggle_pause },
        { "menu",         dvd_vm_bridge_menu },
        { "prev_chapter", dvd_vm_bridge_prev_chapter },
        { "next_chapter", dvd_vm_bridge_next_chapter },
        { "cycle_subs",   dvd_vm_bridge_cycle_subtitles },
        { "cycle_audio",  dvd_vm_bridge_cycle_audio },
    };
    if (!vm || !action) return 0;
    for (size_t i = 0; i < sizeof(simple_actions) / sizeof(simple_actions[0]); i++) {
        if (strcmp(action, simple_actions[i].name) == 0) {
            simple_actions[i].fn(vm);
            return 1;
        }
    }
    if (strcmp(action, "escape") == 0) return dvd_vm_bridge_escape(vm) ? 1 : 0;
    if (strcmp(action, "seek_rel") == 0) { dvd_vm_bridge_seek_relative(vm, (int)darg); return 1; }
    if (strcmp(action, "seek_frac") == 0) { dvd_vm_bridge_seek_fraction(vm, darg); return 1; }
    if (strcmp(action, "mouse_move") == 0) { dvd_vm_bridge_mouse_move(vm, x, y); return 1; }
    if (strcmp(action, "mouse_click") == 0) { dvd_vm_bridge_mouse_click(vm, x, y); return 1; }
    return 0;
}

int tdvd_desktop_vm_action(tdvd_desktop_ctx_t *ctx, const char *action, int x, int y, double darg) {
    if (!ctx || !ctx->bridge || !action) return 0;

    /* The desktop passes mouse positions in window pixels */
    if (strcmp(action, "mouse_move") == 0 || strcmp(action, "mouse_click") == 0) {
        if (!ctx->spu || !ctx->mpv) return 0;
        if (!window_to_dvd(ctx->window, mpv_controller_get_display_aspect(ctx->mpv),
                           spu_compositor_get_frame_height(ctx->spu), x, y, &x, &y)) {
            return 0;
        }
    }
    return dispatch_vm_action(ctx->bridge, action, x, y, darg);
}

/* =========================================================================
 * Cairo / Pango / OpenGL 2D Canvas Primitives
 * ========================================================================= */

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t offset;
} png_mem_reader_t;

static cairo_status_t png_read_from_mem(void *closure, unsigned char *data, unsigned int length) {
    png_mem_reader_t *r = (png_mem_reader_t *)closure;
    if (r->offset + length > r->size) {
        return CAIRO_STATUS_READ_ERROR;
    }
    memcpy(data, r->data + r->offset, length);
    r->offset += length;
    return CAIRO_STATUS_SUCCESS;
}

static void cairo_rounded_rect_path(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (r > w * 0.5) r = w * 0.5;
    if (r > h * 0.5) r = h * 0.5;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI * 0.5, 0.0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0, M_PI * 0.5);
    cairo_arc(cr, x + r, y + h - r, r, M_PI * 0.5, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, M_PI * 1.5);
    cairo_close_path(cr);
}

static void cairo_top_rounded_rect_path(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (r > w * 0.5) r = w * 0.5;
    if (r > h * 0.5) r = h * 0.5;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI * 0.5, 0.0);
    cairo_line_to(cr, x + w, y + h);
    cairo_line_to(cr, x, y + h);
    cairo_arc(cr, x + r, y + r, r, M_PI, M_PI * 1.5);
    cairo_close_path(cr);
}

static double font_cap_center_above_baseline(cairo_t *cr, double font_px, bool bold, bool monospace) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, monospace ? "Monospace" : "Sans");
    pango_font_description_set_absolute_size(desc, font_px * PANGO_SCALE);
    pango_font_description_set_weight(desc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    pango_layout_set_text(layout, "H", 1);
    PangoRectangle ink_rect, logical_rect;
    pango_layout_get_extents(layout, &ink_rect, &logical_rect);
    int baseline = pango_layout_get_baseline(layout);
    g_object_unref(layout);
    double cap_center_y = (double)ink_rect.y + (double)ink_rect.height * 0.5;
    return ((double)baseline - cap_center_y) / (double)PANGO_SCALE;
}

int tdvd_canvas_begin(tdvd_desktop_ctx_t *ctx, int draw_w, int draw_h, int win_w, int win_h) {
    if (!ctx || draw_w <= 0 || draw_h <= 0 || win_w <= 0 || win_h <= 0) return 0;

    if (ctx->active_cr) {
        cairo_destroy(ctx->active_cr);
        ctx->active_cr = NULL;
    }
    if (ctx->active_surf) {
        cairo_surface_destroy(ctx->active_surf);
        ctx->active_surf = NULL;
    }

    if (draw_w != ctx->tex_w || draw_h != ctx->tex_h || !ctx->pixel_buf) {
        free(ctx->pixel_buf);
        ctx->pixel_buf = (uint8_t *)calloc((size_t)draw_w * (size_t)draw_h * 4, 1);
        ctx->tex_w = draw_w;
        ctx->tex_h = draw_h;
    } else {
        memset(ctx->pixel_buf, 0, (size_t)draw_w * (size_t)draw_h * 4);
    }

    if (!ctx->pixel_buf) {
        ctx->tex_w = 0;
        ctx->tex_h = 0;
        return 0;
    }

    ctx->active_surf = cairo_image_surface_create_for_data(
        ctx->pixel_buf, CAIRO_FORMAT_ARGB32, draw_w, draw_h, draw_w * 4);
    if (!ctx->active_surf || cairo_surface_status(ctx->active_surf) != CAIRO_STATUS_SUCCESS) {
        if (ctx->active_surf) cairo_surface_destroy(ctx->active_surf);
        ctx->active_surf = NULL;
        return 0;
    }
    ctx->active_cr = cairo_create(ctx->active_surf);
    if (!ctx->active_cr || cairo_status(ctx->active_cr) != CAIRO_STATUS_SUCCESS) {
        if (ctx->active_cr) cairo_destroy(ctx->active_cr);
        ctx->active_cr = NULL;
        cairo_surface_destroy(ctx->active_surf);
        ctx->active_surf = NULL;
        return 0;
    }

    double sx = (double)draw_w / (double)win_w;
    double sy = (double)draw_h / (double)win_h;
    cairo_scale(ctx->active_cr, sx, sy);
    return 1;
}

void tdvd_canvas_end(tdvd_desktop_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->active_cr) {
        cairo_destroy(ctx->active_cr);
        ctx->active_cr = NULL;
    }
    if (ctx->active_surf) {
        cairo_surface_flush(ctx->active_surf);
        cairo_surface_destroy(ctx->active_surf);
        ctx->active_surf = NULL;
    }
    if (!ctx->pixel_buf || ctx->tex_w <= 0 || ctx->tex_h <= 0) return;

    if (!ctx->gl_initialized) {
        glGenTextures(1, &ctx->gl_tex);
        glBindTexture(GL_TEXTURE_2D, ctx->gl_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        ctx->gl_initialized = true;
    } else {
        glBindTexture(GL_TEXTURE_2D, ctx->gl_tex);
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ctx->tex_w, ctx->tex_h, 0, GL_BGRA,
                 GL_UNSIGNED_INT_8_8_8_8_REV, ctx->pixel_buf);
}

void tdvd_canvas_save(tdvd_desktop_ctx_t *ctx) {
    if (ctx && ctx->active_cr) cairo_save(ctx->active_cr);
}

void tdvd_canvas_restore(tdvd_desktop_ctx_t *ctx) {
    if (ctx && ctx->active_cr) cairo_restore(ctx->active_cr);
}

void tdvd_canvas_clip_rect(tdvd_desktop_ctx_t *ctx, double x, double y, double w, double h,
                           double radius, int top_only) {
    if (!ctx || !ctx->active_cr || w <= 0 || h <= 0) return;
    cairo_t *cr = ctx->active_cr;
    if (radius > 0.0) {
        if (top_only) cairo_top_rounded_rect_path(cr, x, y, w, h, radius);
        else cairo_rounded_rect_path(cr, x, y, w, h, radius);
    } else {
        cairo_rectangle(cr, x, y, w, h);
    }
    cairo_clip(cr);
}

void tdvd_canvas_radial_bg(tdvd_desktop_ctx_t *ctx, int win_w, int win_h,
                           double r0, double g0, double b0,
                           double r1, double g1, double b1) {
    if (!ctx || !ctx->active_cr) return;
    cairo_t *cr = ctx->active_cr;
    cairo_pattern_t *bg = cairo_pattern_create_radial(
        win_w * 0.5, 0.0, 20.0, win_w * 0.5, 0.0, fmax(win_w, win_h));
    cairo_pattern_add_color_stop_rgb(bg, 0.0, r0, g0, b0);
    cairo_pattern_add_color_stop_rgb(bg, 1.0, r1, g1, b1);
    cairo_set_source(cr, bg);
    cairo_paint(cr);
    cairo_pattern_destroy(bg);
}

void tdvd_canvas_linear_gradient_rect(tdvd_desktop_ctx_t *ctx, double x, double y, double w, double h,
                                      double r0, double g0, double b0, double a0,
                                      double r1, double g1, double b1, double a1) {
    if (!ctx || !ctx->active_cr || w <= 0 || h <= 0) return;
    cairo_t *cr = ctx->active_cr;
    cairo_pattern_t *pat = cairo_pattern_create_linear(x, y, x, y + h);
    cairo_pattern_add_color_stop_rgba(pat, 0.0, r0, g0, b0, a0);
    cairo_pattern_add_color_stop_rgba(pat, 1.0, r1, g1, b1, a1);
    cairo_set_source(cr, pat);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);
}

void tdvd_canvas_rounded_rect(tdvd_desktop_ctx_t *ctx, double x, double y, double w, double h,
                              double radius, int top_only,
                              double fill_r, double fill_g, double fill_b, double fill_a,
                              double stroke_w, double stroke_r, double stroke_g, double stroke_b, double stroke_a) {
    if (!ctx || !ctx->active_cr || w <= 0 || h <= 0) return;
    cairo_t *cr = ctx->active_cr;
    cairo_save(cr);
    if (radius > 0.0) {
        if (top_only) cairo_top_rounded_rect_path(cr, x, y, w, h, radius);
        else cairo_rounded_rect_path(cr, x, y, w, h, radius);
    } else {
        cairo_rectangle(cr, x, y, w, h);
    }

    bool has_fill = (fill_a > 0.0);
    bool has_stroke = (stroke_w > 0.0 && stroke_a > 0.0);
    if (has_fill) {
        cairo_set_source_rgba(cr, fill_r, fill_g, fill_b, fill_a);
        if (has_stroke) cairo_fill_preserve(cr);
        else cairo_fill(cr);
    }
    if (has_stroke) {
        cairo_set_source_rgba(cr, stroke_r, stroke_g, stroke_b, stroke_a);
        cairo_set_line_width(cr, stroke_w);
        cairo_stroke(cr);
    } else if (!has_fill) {
        cairo_new_path(cr);
    }
    cairo_restore(cr);
}

double tdvd_canvas_text(tdvd_desktop_ctx_t *ctx, const char *text, double x, double y, double w,
                        int max_lines, double font_px, int bold, int monospace,
                        double r, double g, double b, double a, int align, int vcenter) {
    if (!ctx || !ctx->active_cr) return 0.0;
    cairo_t *cr = ctx->active_cr;

    int eff_lines = max_lines;
    if (vcenter && eff_lines <= 0) {
        eff_lines = 1;
    }

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, monospace ? "Monospace" : "Sans");
    pango_font_description_set_absolute_size(desc, font_px * PANGO_SCALE);
    pango_font_description_set_weight(desc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    pango_layout_set_text(layout, text ? text : "", -1);
    if (w > 0.0) {
        pango_layout_set_width(layout, (int)(w * PANGO_SCALE));
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        if (eff_lines > 0) {
            pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
            pango_layout_set_height(layout, -eff_lines);
        }
    }
    PangoAlignment palign = PANGO_ALIGN_LEFT;
    if (align == 1) palign = PANGO_ALIGN_CENTER;
    else if (align == 2) palign = PANGO_ALIGN_RIGHT;
    pango_layout_set_alignment(layout, palign);

    double draw_y = y;
    if (vcenter) {
        double layout_baseline = (double)pango_layout_get_baseline(layout) / (double)PANGO_SCALE;
        double cap_above_bl = font_cap_center_above_baseline(cr, font_px, bold != 0, monospace != 0);
        draw_y = y - (layout_baseline - cap_above_bl);
    }

    int lw = 0, lh = 0;
    pango_layout_get_pixel_size(layout, &lw, &lh);

    cairo_save(cr);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_move_to(cr, x, draw_y);
    pango_cairo_show_layout(cr, layout);
    cairo_new_path(cr);
    cairo_restore(cr);

    g_object_unref(layout);
    return (double)lh;
}

double tdvd_canvas_measure_text(tdvd_desktop_ctx_t *ctx, const char *text, double font_px,
                                int bold, int monospace) {
    if (!text || !text[0]) return 0.0;

    cairo_surface_t *tmp_surf = NULL;
    cairo_t *tmp_cr = NULL;
    cairo_t *cr = (ctx && ctx->active_cr) ? ctx->active_cr : NULL;
    if (!cr) {
        tmp_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        tmp_cr = cairo_create(tmp_surf);
        cr = tmp_cr;
    }

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, monospace ? "Monospace" : "Sans");
    pango_font_description_set_absolute_size(desc, font_px * PANGO_SCALE);
    pango_font_description_set_weight(desc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    pango_layout_set_text(layout, text, -1);
    int lw = 0, lh = 0;
    pango_layout_get_pixel_size(layout, &lw, &lh);
    g_object_unref(layout);

    if (tmp_cr) cairo_destroy(tmp_cr);
    if (tmp_surf) cairo_surface_destroy(tmp_surf);
    return (double)lw;
}

int tdvd_canvas_text_x_to_byte_index(tdvd_desktop_ctx_t *ctx, const char *text, double font_px,
                                     int bold, int monospace, double target_x, int *out_trailing) {
    if (out_trailing) *out_trailing = 0;
    if (!text || !text[0] || target_x <= 0.0) return 0;

    cairo_surface_t *tmp_surf = NULL;
    cairo_t *tmp_cr = NULL;
    cairo_t *cr = (ctx && ctx->active_cr) ? ctx->active_cr : NULL;
    if (!cr) {
        tmp_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        tmp_cr = cairo_create(tmp_surf);
        cr = tmp_cr;
    }

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, monospace ? "Monospace" : "Sans");
    pango_font_description_set_absolute_size(desc, font_px * PANGO_SCALE);
    pango_font_description_set_weight(desc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    int tlen = (int)strlen(text);
    pango_layout_set_text(layout, text, tlen);

    int idx = 0;
    int trailing = 0;
    gboolean inside = pango_layout_xy_to_index(layout, (int)(target_x * PANGO_SCALE), 0, &idx, &trailing);
    if (!inside) {
        int lw = 0, lh = 0;
        pango_layout_get_pixel_size(layout, &lw, &lh);
        if (target_x >= (double)lw) {
            idx = tlen;
            trailing = 0;
        }
    }
    if (idx < 0) idx = 0;
    if (idx > tlen) idx = tlen;
    if (out_trailing) *out_trailing = trailing;

    g_object_unref(layout);
    if (tmp_cr) cairo_destroy(tmp_cr);
    if (tmp_surf) cairo_surface_destroy(tmp_surf);
    return idx;
}

void tdvd_canvas_draw_log_block(tdvd_desktop_ctx_t *ctx, const char *text,
                                double x, double y, double w, double h,
                                double font_px, double scroll_offset,
                                double r, double g, double b, double a) {
    if (!ctx || !ctx->active_cr || w <= 4.0 || h <= 4.0) return;
    cairo_t *cr = ctx->active_cr;

    cairo_save(cr);
    cairo_rounded_rect_path(cr, x, y, w, h, 6.0);
    cairo_set_source_rgb(cr, 0.024, 0.027, 0.039);
    cairo_fill_preserve(cr);
    cairo_clip(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, "Monospace");
    pango_font_description_set_absolute_size(desc, font_px * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_text(layout, text ? text : "", -1);
    pango_layout_set_width(layout, (int)(fmax(16.0, w - 16.0) * PANGO_SCALE));
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);

    int lw = 0, lh = 0;
    pango_layout_get_pixel_size(layout, &lw, &lh);
    double draw_y = y + 6.0;
    if ((double)lh > h - 12.0) {
        draw_y = (y + h - 6.0) - (double)lh + scroll_offset;
        if (draw_y > y + 6.0) draw_y = y + 6.0;
    }
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_move_to(cr, x + 8.0, draw_y);
    pango_cairo_show_layout(cr, layout);
    cairo_new_path(cr);
    g_object_unref(layout);
    cairo_restore(cr);
}

void tdvd_canvas_search_icon(tdvd_desktop_ctx_t *ctx, double cx, double cy,
                             double r, double g, double b, double a) {
    if (!ctx || !ctx->active_cr) return;
    cairo_t *cr = ctx->active_cr;
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_set_line_width(cr, 1.6);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_arc(cr, cx - 1.2, cy - 1.2, 5.0, 0.0, 2.0 * M_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, cx + 2.4, cy + 2.4);
    cairo_line_to(cr, cx + 6.0, cy + 6.0);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void tdvd_canvas_x_icon(tdvd_desktop_ctx_t *ctx, double cx, double cy,
                        double half_sz, double stroke_w,
                        double r, double g, double b, double a) {
    if (!ctx || !ctx->active_cr) return;
    cairo_t *cr = ctx->active_cr;
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_set_line_width(cr, stroke_w);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, cx - half_sz, cy - half_sz);
    cairo_line_to(cr, cx + half_sz, cy + half_sz);
    cairo_move_to(cr, cx + half_sz, cy - half_sz);
    cairo_line_to(cr, cx - half_sz, cy + half_sz);
    cairo_stroke(cr);
    cairo_restore(cr);
}

int tdvd_canvas_cache_png(tdvd_desktop_ctx_t *ctx, const char *cache_key,
                          const uint8_t *png_data, size_t png_len, double sar) {
    if (!ctx || !cache_key || !cache_key[0] || !png_data || png_len == 0) return 0;

    for (int i = 0; i < ctx->png_cache_count; i++) {
        if (strcmp(ctx->png_cache[i].key, cache_key) == 0) {
            return ctx->png_cache[i].surface ? 1 : 0;
        }
    }
    if (ctx->png_cache_count >= MAX_PNG_CACHE) return 0;

    png_mem_reader_t reader = { .data = png_data, .size = png_len, .offset = 0 };
    cairo_surface_t *s = cairo_image_surface_create_from_png_stream(png_read_from_mem, &reader);
    if (!s || cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        if (s) cairo_surface_destroy(s);
        return 0;
    }

    tdvd_png_cache_entry_t *entry = &ctx->png_cache[ctx->png_cache_count++];
    snprintf(entry->key, sizeof(entry->key), "%s", cache_key);
    entry->surface = s;
    entry->sar = (sar > 0.0) ? sar : 1.0;
    return 1;
}

int tdvd_canvas_draw_cached_png(tdvd_desktop_ctx_t *ctx, const char *cache_key,
                                double x, double y, double w, double h) {
    if (!ctx || !ctx->active_cr || !cache_key || !cache_key[0] || w <= 0.0 || h <= 0.0) return 0;
    for (int i = 0; i < ctx->png_cache_count; i++) {
        if (strcmp(ctx->png_cache[i].key, cache_key) == 0) {
            cairo_surface_t *tsurf = ctx->png_cache[i].surface;
            if (!tsurf) return 0;
            int iw = cairo_image_surface_get_width(tsurf);
            int ih = cairo_image_surface_get_height(tsurf);
            if (iw <= 0 || ih <= 0) return 0;

            double sar = (ctx->png_cache[i].sar > 0.0) ? ctx->png_cache[i].sar : 1.0;
            double eff_w = (double)iw * sar;
            double eff_h = (double)ih;
            double sx = w / eff_w;
            double sy = h / eff_h;
            double sc = fmax(sx, sy);
            double dw = eff_w * sc;
            double dh = eff_h * sc;
            double ox = x + (w - dw) * 0.5;
            double oy = y + (h - dh) * 0.5;

            cairo_t *cr = ctx->active_cr;
            cairo_save(cr);
            cairo_translate(cr, ox, oy);
            cairo_scale(cr, sc * sar, sc);
            cairo_set_source_surface(cr, tsurf, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
            return 1;
        }
    }
    return 0;
}

void tdvd_desktop_present_frame(tdvd_desktop_ctx_t *ctx, int in_player_view, int draw_ui_overlay) {
    if (!ctx || !ctx->window) return;

    mpv_controller_poll_events(ctx->mpv);

    int draw_w = 0, draw_h = 0;
    SDL_GL_GetDrawableSize(ctx->window, &draw_w, &draw_h);
    if (draw_w <= 0 || draw_h <= 0) {
        SDL_Delay(10);
        return;
    }

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (in_player_view) {
        mpv_controller_render(ctx->mpv, draw_w, draw_h);
        spu_compositor_render_gl(ctx->spu, draw_w, draw_h, mpv_controller_get_display_aspect(ctx->mpv));
    }

    if (draw_ui_overlay && ctx->gl_initialized) {
        glUseProgram(0);
        glActiveTexture(GL_TEXTURE0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);

        glViewport(0, 0, draw_w, draw_h);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, ctx->gl_tex);

        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glOrtho(0, draw_w, draw_h, 0, -1, 1);

        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();

        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(0.0f,          0.0f);
        glTexCoord2f(1.0f, 0.0f); glVertex2f((float)draw_w, 0.0f);
        glTexCoord2f(1.0f, 1.0f); glVertex2f((float)draw_w, (float)draw_h);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(0.0f,          (float)draw_h);
        glEnd();

        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_MODELVIEW);

        glDisable(GL_TEXTURE_2D);
        glDisable(GL_BLEND);
    }

    SDL_GL_SwapWindow(ctx->window);
    SDL_Delay(10);
}

/* =========================================================================
 * Part B: Web / Headless DVD VM Session Bridge
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

    /* Stop the old VM without holding s->lock: stopping joins its thread, and
     * that thread may be blocked in a callback that is waiting on a client */
    pthread_mutex_lock(&s->lock);
    dvd_vm_bridge_t *old_vm = s->vm;
    s->vm = NULL;
    pthread_mutex_unlock(&s->lock);
    if (old_vm) dvd_vm_bridge_destroy(old_vm);

    pthread_mutex_lock(&s->lock);
    if (s->vm) {
        /* A concurrent load won the race; replace its VM */
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
    /* Web clients send mouse positions already in DVD frame pixels */
    dispatch_vm_action(s->vm, action, x, y, 0.0);
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
