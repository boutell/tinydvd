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

#include "mvob_parser.h"
#include "mpv_controller.h"
#include "spu_compositor.h"
#include "dvd_vm_bridge.h"

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

static void print_usage(const char *prog) {
    printf("tinydvd Player v%s\n", TINYDVD_VERSION);
    printf("Interactive DVD-Video Hybrid Player (.tdvd / H.264 + Meta-VOB)\n\n");
    printf("Usage: %s [options] <movie.tdvd | tinydvd_directory>\n\n", prog);
    printf("Options:\n");
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

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("tinydvd v%s\n", TINYDVD_VERSION);
            return 0;
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

    if (!dvd_path) {
        fprintf(stderr, "Error: No DVD path specified.\n\n");
        print_usage(argv[0]);
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
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

    SDL_Window *window = SDL_CreateWindow(
        "tinydvd Player",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        853, 480, /* 16:9 widescreen or 720:480 display aspect */
        window_flags
    );

    if (!window) {
        fprintf(stderr, "Failed to create SDL window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) {
        fprintf(stderr, "Failed to create OpenGL context: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_GL_MakeCurrent(window, gl_ctx);
    SDL_GL_SetSwapInterval(1); /* vsync */

    /* Initialize subsystems */
    mpv_controller_t *mpv = mpv_controller_init(get_proc_address_sdl, on_mpv_update_cb, NULL);
    if (!mpv) {
        fprintf(stderr, "Failed to initialize mpv_controller\n");
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    spu_compositor_t *spu = spu_compositor_create();
    if (!spu) {
        fprintf(stderr, "Failed to initialize spu_compositor\n");
        mpv_controller_destroy(mpv);
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    dvd_vm_bridge_t *bridge = dvd_vm_bridge_init(dvd_path, mpv, spu);
    if (!bridge) {
        fprintf(stderr, "Failed to initialize DVD VM bridge for '%s'\n", dvd_path);
        spu_compositor_destroy(spu);
        mpv_controller_destroy(mpv);
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    dvd_vm_bridge_start(bridge);

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    bool running = true;
    bool is_fullscreen = start_fullscreen;
    bool cursor_visible = true;
    Uint32 last_mouse_activity = SDL_GetTicks();

    while (running && !g_quit_requested && dvd_vm_bridge_is_running(bridge)) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_MOUSEMOTION || ev.type == SDL_MOUSEBUTTONDOWN ||
                ev.type == SDL_MOUSEBUTTONUP || ev.type == SDL_MOUSEWHEEL) {
                last_mouse_activity = SDL_GetTicks();
                if (!cursor_visible) {
                    SDL_ShowCursor(SDL_ENABLE);
                    cursor_visible = true;
                }
            }
            switch (ev.type) {
                case SDL_QUIT:
                    running = false;
                    break;

                case SDL_KEYDOWN:
                    switch (ev.key.keysym.sym) {
                        case SDLK_q:
                            running = false;
                            break;
                        case SDLK_UP:
                            dvd_vm_bridge_up_button(bridge);
                            break;
                        case SDLK_DOWN:
                            dvd_vm_bridge_down_button(bridge);
                            break;
                        case SDLK_LEFT:
                            if (ev.key.keysym.mod & KMOD_SHIFT) {
                                dvd_vm_bridge_seek_relative(bridge, -60);
                            } else {
                                dvd_vm_bridge_left_button(bridge);
                            }
                            break;
                        case SDLK_RIGHT:
                            if (ev.key.keysym.mod & KMOD_SHIFT) {
                                dvd_vm_bridge_seek_relative(bridge, +60);
                            } else {
                                dvd_vm_bridge_right_button(bridge);
                            }
                            break;
                        case SDLK_COMMA:
                            dvd_vm_bridge_seek_relative(bridge, -10);
                            break;
                        case SDLK_PERIOD:
                            dvd_vm_bridge_seek_relative(bridge, +10);
                            break;
                        case SDLK_LEFTBRACKET:
                        case SDLK_PAGEUP:
                            dvd_vm_bridge_prev_chapter(bridge);
                            break;
                        case SDLK_RIGHTBRACKET:
                        case SDLK_PAGEDOWN:
                            dvd_vm_bridge_next_chapter(bridge);
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
                            int digit = (int)(ev.key.keysym.sym - SDLK_0);
                            dvd_vm_bridge_seek_fraction(bridge, (double)digit * 0.10);
                            break;
                        }
                        case SDLK_c:
                        case SDLK_s:
                            dvd_vm_bridge_cycle_subtitles(bridge);
                            break;
                        case SDLK_a:
                            dvd_vm_bridge_cycle_audio(bridge);
                            break;
                        case SDLK_RETURN:
                        case SDLK_KP_ENTER:
                            dvd_vm_bridge_activate_button(bridge);
                            break;
                        case SDLK_SPACE:
                            dvd_vm_bridge_toggle_pause(bridge);
                            break;
                        case SDLK_m:
                            dvd_vm_bridge_menu(bridge);
                            break;
                        case SDLK_ESCAPE:
                            if (!dvd_vm_bridge_escape(bridge)) {
                                running = false;
                            }
                            break;
                        case SDLK_f:
                            is_fullscreen = !is_fullscreen;
                            SDL_SetWindowFullscreen(window, is_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                            break;
                        default:
                            break;
                    }
                    break;

                case SDL_MOUSEMOTION: {
                    int x_dvd, y_dvd;
                    if (window_to_dvd(window, mpv_controller_get_display_aspect(mpv),
                                      spu_compositor_get_frame_height(spu), ev.motion.x, ev.motion.y, &x_dvd, &y_dvd)) {
                        dvd_vm_bridge_mouse_move(bridge, x_dvd, y_dvd);
                    }
                    break;
                }

                case SDL_MOUSEBUTTONDOWN: {
                    int x_dvd, y_dvd;
                    if (ev.button.button == SDL_BUTTON_LEFT &&
                        window_to_dvd(window, mpv_controller_get_display_aspect(mpv),
                                      spu_compositor_get_frame_height(spu), ev.button.x, ev.button.y, &x_dvd, &y_dvd)) {
                        dvd_vm_bridge_mouse_click(bridge, x_dvd, y_dvd);
                    }
                    break;
                }

                default:
                    break;
            }
        }

        if (cursor_visible && SDL_GetTicks() - last_mouse_activity >= CURSOR_IDLE_MS) {
            SDL_ShowCursor(SDL_DISABLE);
            cursor_visible = false;
        }

        mpv_controller_poll_events(mpv);

        /* Render pass */
        int draw_w, draw_h;
        SDL_GL_GetDrawableSize(window, &draw_w, &draw_h);

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        mpv_controller_render(mpv, draw_w, draw_h);
        spu_compositor_render_gl(spu, draw_w, draw_h, mpv_controller_get_display_aspect(mpv));

        SDL_GL_SwapWindow(window);

        SDL_Delay(10);
    }

    printf("[main] Cleaning up...\n");
    dvd_vm_bridge_destroy(bridge);
    spu_compositor_destroy(spu);
    mpv_controller_destroy(mpv);
    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
