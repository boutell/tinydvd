#ifndef TINYDVD_NATIVE_UI_H
#define TINYDVD_NATIVE_UI_H

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stddef.h>

#include "library.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct native_ui native_ui_t;

native_ui_t *native_ui_create(SDL_Window *window, tdvd_library_t *lib);
void native_ui_destroy(native_ui_t *ui);

bool native_ui_is_modal_open(const native_ui_t *ui);
void native_ui_mark_dirty(native_ui_t *ui);

/**
 * Handle an SDL event for the Library View, Import Modal, or Player HUD.
 * Returns true if the UI consumed the event (so player shortcuts/DVD menu clicks should ignore it).
 */
bool native_ui_handle_event(native_ui_t *ui, const SDL_Event *ev,
                            bool in_player_view,
                            char *out_play_disc_path, size_t path_len,
                            char *out_play_disc_title, size_t title_len,
                            bool *out_back_to_library);

/**
 * Render either the full Library View (+ Import modal) or Player HUD (+ Import modal)
 * into the active OpenGL context at drawable size (draw_w x draw_h).
 */
void native_ui_render(native_ui_t *ui, int draw_w, int draw_h,
                      bool in_player_view, const char *current_movie_title,
                      bool hud_visible);

#ifdef __cplusplus
}
#endif

#endif /* TINYDVD_NATIVE_UI_H */
