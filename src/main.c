#include <SDL2/SDL.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mvob_parser.h"
#include "mpv_controller.h"
#include "spu_compositor.h"
#include "dvd_vm_bridge.h"
#include "library.h"
#include "ui/native_ui.h"
#include "web_server.h"

#define TINYDVD_VERSION "1.0.0"
#define WAKEUP_EVENT (SDL_USEREVENT + 1)
/* Hide the mouse cursor after this long without mouse activity, like most video players */
#define CURSOR_IDLE_MS 2000

static volatile sig_atomic_t g_quit_requested = 0;

static void handle_sigint(int sig) {
    (void)sig;
    g_quit_requested = 1;
}

static void *get_proc_address_sdl(void *ctx, const char *name) {
    (void)ctx;
    return SDL_GL_GetProcAddress(name);
}

/* Map a window position to DVD menu coordinates (720 x 480 for NTSC, 720 x 576
 * for PAL), over the video as mpv letterboxes it at its display aspect */
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

static void resolve_default_library_path(char *out, size_t out_len) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        snprintf(out, out_len, ".");
        return;
    }
    char movies[1024], videos[1024];
    snprintf(movies, sizeof(movies), "%s/Movies", home);
    snprintf(videos, sizeof(videos), "%s/Videos", home);
    struct stat st;
    if (stat(movies, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(out, out_len, "%s", movies);
    } else if (stat(videos, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(out, out_len, "%s", videos);
    } else {
#ifdef __APPLE__
        snprintf(out, out_len, "%s", movies);
#else
        snprintf(out, out_len, "%s", videos);
#endif
    }
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

static void print_usage(const char *prog) {
    printf("tinydvd Player v%s\n", TINYDVD_VERSION);
    printf("Interactive DVD-Video Hybrid Player (.tdvd / H.264 + Meta-VOB)\n\n");
    printf("Usage: %s [options] [movie.tdvd | library_directory]\n\n", prog);
    printf("Options:\n");
    printf("  -s, --serve          Start embedded web streaming server\n");
    printf("  -p, --port <port>    Web server port (default: 8080)\n");
    printf("  -b, --bind <ip>      Web server bind address (default: 0.0.0.0)\n");
    printf("  -f, --fullscreen     Start in fullscreen mode\n");
    printf("  -h, --help           Show this help text\n");
    printf("  -v, --version        Show version information\n\n");
    printf("Controls:\n");
    printf("  Arrow Keys (in menu) Navigate menu buttons\n");
    printf("  Enter / Space        Activate selected menu button\n");
    printf("  Mouse Click / Move   Direct DVD menu button selection\n");
    printf("  Left / Right Arrow   Seek -10s / +10s along clip timeline\n");
    printf("  Shift + Left / Right Seek -60s / +60s along clip timeline\n");
    printf("  Down / Up Arrow      Seek -60s / +60s along clip timeline\n");
    printf("  , / . (Comma/Period) Seek -10s / +10s along clip timeline\n");
    printf("  [ / ] or PgUp / PgDn Previous / Next Chapter\n");
    printf("  0 - 9                Jump to 0%% - 90%% of clip timeline\n");
    printf("  C / S                Cycle Closed Captions / Subtitles (Track 1..N, Off)\n");
    printf("  A                    Cycle Audio tracks\n");
    printf("  Space (in film)      Toggle pause\n");
    printf("  M                    Jump to DVD Root Menu\n");
    printf("  Escape               Go up a menu level (or exit if at root menu)\n");
    printf("  F                    Toggle fullscreen\n");
    printf("  Q                    Quit\n");
}

int main(int argc, char *argv[]) {
    const char *dvd_path = NULL;
    bool start_fullscreen = false;
    bool serve_mode = false;
    int port = 8080;
    const char *bind_addr = "0.0.0.0";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("tinydvd v%s\n", TINYDVD_VERSION);
            return 0;
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--serve") == 0) {
            serve_mode = true;
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (i + 1 < argc) {
                port = atoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--bind") == 0) {
            if (i + 1 < argc) {
                bind_addr = argv[++i];
            }
        } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--fullscreen") == 0) {
            start_fullscreen = true;
        } else if (argv[i][0] != '-') {
            dvd_path = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    char default_lib_buf[1024] = {0};
    if (!dvd_path) {
        if (serve_mode) {
            fprintf(stderr, "Error: No DVD path specified.\n\n");
            print_usage(argv[0]);
            return 1;
        }
        resolve_default_library_path(default_lib_buf, sizeof(default_lib_buf));
        dvd_path = default_lib_buf;
    }

    if (serve_mode) {
        signal(SIGINT, handle_sigint);
        signal(SIGTERM, handle_sigint);
        web_server_config_t config = {
            .bind_addr = bind_addr,
            .port = port,
            .target_path = dvd_path
        };
        web_server_t *server = web_server_start(&config);
        if (!server) {
            fprintf(stderr, "Failed to start web server.\n");
            return 1;
        }
        while (!g_quit_requested) {
            sleep(1);
        }
        printf("\nStopping web server...\n");
        web_server_destroy(server);
        return 0;
    }

    tdvd_library_t lib;
    tdvd_library_init(&lib, dvd_path);

    char library_fallback_dir[1024] = {0};
    if (!lib.is_library) {
        snprintf(library_fallback_dir, sizeof(library_fallback_dir), "%s", lib.target_path);
        char *sl = strrchr(library_fallback_dir, '/');
        if (sl && sl > library_fallback_dir) *sl = '\0';
        else if (sl == library_fallback_dir) library_fallback_dir[1] = '\0';
        else snprintf(library_fallback_dir, sizeof(library_fallback_dir), ".");
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        tdvd_library_destroy(&lib);
        return 1;
    }

    /* Configure OpenGL attributes */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    Uint32 window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN |
                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (start_fullscreen) {
        window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

    int init_w = lib.is_library ? 1280 : 853;
    int init_h = lib.is_library ? 800 : 480;
    const char *init_title = lib.is_library ? "TinyDVD Cinema" : "tinydvd Player";

    SDL_Window *window = SDL_CreateWindow(
        init_title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        init_w, init_h,
        window_flags
    );

    if (!window) {
        fprintf(stderr, "Failed to create SDL window: %s\n", SDL_GetError());
        tdvd_library_destroy(&lib);
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) {
        fprintf(stderr, "Failed to create OpenGL context: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        tdvd_library_destroy(&lib);
        SDL_Quit();
        return 1;
    }

    SDL_GL_MakeCurrent(window, gl_ctx);
    SDL_GL_SetSwapInterval(1); /* vsync */
    SDL_StartTextInput();

    /* Prevent screensaver / screen blanking / idle sleep during playback */
    SDL_DisableScreenSaver();

    /* Initialize subsystems */
    mpv_controller_t *mpv = mpv_controller_init(get_proc_address_sdl, on_mpv_update_cb, NULL);
    if (!mpv) {
        fprintf(stderr, "Failed to initialize mpv_controller\n");
        SDL_StopTextInput();
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        tdvd_library_destroy(&lib);
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
        tdvd_library_destroy(&lib);
        SDL_Quit();
        return 1;
    }

    native_ui_t *ui = native_ui_create(window, &lib);
    dvd_vm_bridge_t *bridge = NULL;
    bool in_player_view = false;
    char current_movie_title[256] = {0};

    if (!lib.is_library) {
        const char *play_target = (lib.catalog_count > 0) ? lib.catalog[0].path : lib.target_path;
        const char *play_title = (lib.catalog_count > 0) ? lib.catalog[0].title : dvd_path;
        if (!start_movie_playback(window, mpv, &spu, &bridge, play_target, play_title,
                                  current_movie_title, sizeof(current_movie_title),
                                  &in_player_view, ui)) {
            if (ui) native_ui_destroy(ui);
            if (spu) spu_compositor_destroy(spu);
            mpv_controller_destroy(mpv);
            SDL_StopTextInput();
            SDL_GL_DeleteContext(gl_ctx);
            SDL_DestroyWindow(window);
            tdvd_library_destroy(&lib);
            SDL_Quit();
            return 1;
        }
    }

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    bool running = true;
    bool is_fullscreen = start_fullscreen;
    bool cursor_visible = true;
    Uint32 last_mouse_activity = SDL_GetTicks();

    while (running && !g_quit_requested) {
        if (in_player_view && bridge && !dvd_vm_bridge_is_running(bridge)) {
            return_to_library(window, mpv, &spu, &bridge, &lib, library_fallback_dir,
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
                    return_to_library(window, mpv, &spu, &bridge, &lib, library_fallback_dir,
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
                                    return_to_library(window, mpv, &spu, &bridge, &lib, library_fallback_dir,
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

                case SDL_MOUSEMOTION: {
                    if (in_player_view && bridge && spu) {
                        int x_dvd, y_dvd;
                        if (window_to_dvd(window, mpv_controller_get_display_aspect(mpv),
                                          spu_compositor_get_frame_height(spu), ev.motion.x, ev.motion.y, &x_dvd, &y_dvd)) {
                            dvd_vm_bridge_mouse_move(bridge, x_dvd, y_dvd);
                        }
                    }
                    break;
                }

                case SDL_MOUSEBUTTONDOWN: {
                    if (in_player_view && bridge && spu) {
                        int x_dvd, y_dvd;
                        if (ev.button.button == SDL_BUTTON_LEFT &&
                            window_to_dvd(window, mpv_controller_get_display_aspect(mpv),
                                          spu_compositor_get_frame_height(spu), ev.button.x, ev.button.y, &x_dvd, &y_dvd)) {
                            dvd_vm_bridge_mouse_click(bridge, x_dvd, y_dvd);
                        }
                    }
                    break;
                }

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

        /* Render pass */
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
    tdvd_library_destroy(&lib);
    SDL_Quit();

    return 0;
}
