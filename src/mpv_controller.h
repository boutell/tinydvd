#ifndef MPV_CONTROLLER_H
#define MPV_CONTROLLER_H

#include <stdint.h>
#include <stdbool.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include "player_adapter.h"

typedef struct mpv_controller mpv_controller_t;

typedef void (*mpv_update_callback_t)(void *user_data);

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize mpv_controller.
 * @param get_proc_address Function pointer for OpenGL loader (e.g. SDL_GL_GetProcAddress)
 * @param on_update Optional callback invoked on MPV render updates
 * @param user_data User context for callback
 */
mpv_controller_t *mpv_controller_init(void *(*get_proc_address)(void *, const char *),
                                     mpv_update_callback_t on_update,
                                     void *user_data);

/**
 * Destroy mpv_controller instance.
 */
void mpv_controller_destroy(mpv_controller_t *ctrl);

/*
 * Playback commands (load, seek, pause, audio/subtitle tracks, OSD) and
 * playback state are reached through the player_adapter returned by
 * mpv_controller_get_adapter(); this header only covers rendering and events.
 */

/**
 * Render the current video frame into the active OpenGL viewport / FBO.
 */
void mpv_controller_render(mpv_controller_t *ctrl, int width, int height);

/**
 * Poll mpv events and forward them to the player adapter. Call regularly
 * from the main loop.
 */
void mpv_controller_poll_events(mpv_controller_t *ctrl);

/**
 * Check if a new video frame is ready to render.
 */
bool mpv_controller_needs_render(mpv_controller_t *ctrl);

/**
 * Display aspect ratio (width / height) of the current video, as mpv shows it:
 * 4:3 or 16:9 depending on the menu or title. 16:9 until mpv knows.
 */
float mpv_controller_get_display_aspect(mpv_controller_t *ctrl);

/**
 * Underlying mpv handle.
 */
mpv_handle *mpv_controller_get_handle(mpv_controller_t *ctrl);

/**
 * Get the unified player adapter backed by this mpv_controller.
 */
player_adapter_t *mpv_controller_get_adapter(mpv_controller_t *ctrl);

#ifdef __cplusplus
}
#endif

#endif /* MPV_CONTROLLER_H */
