#ifndef MPV_CONTROLLER_H
#define MPV_CONTROLLER_H

#include <stdint.h>
#include <stdbool.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>

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

/**
 * Load an MKV video file and optional companion audio file (or .mvob).
 */
int mpv_controller_load_file(mpv_controller_t *ctrl, const char *video_path, const char *audio_path);

/**
 * Select 1-based audio track ID in mpv (or 0 to disable audio).
 */
void mpv_controller_set_audio_track(mpv_controller_t *ctrl, int aid);

/**
 * Select 1-based internal subtitle/CC track ID in mpv (or 0 to disable).
 */
void mpv_controller_set_subtitle_track(mpv_controller_t *ctrl, int sid);

/**
 * Display a temporary On-Screen Display (OSD) status message in the video window.
 */
void mpv_controller_osd_message(mpv_controller_t *ctrl, const char *msg);

/**
 * Perform exact asynchronous seek to pts_nanos.
 */
void mpv_controller_seek_exact(mpv_controller_t *ctrl, uint64_t pts_nanos);

/**
 * Pause or resume playback.
 */
void mpv_controller_pause(mpv_controller_t *ctrl, bool pause);

/**
 * Query whether playback is currently paused.
 */
bool mpv_controller_is_paused(mpv_controller_t *ctrl);

/**
 * Get current presentation timestamp in nanoseconds.
 */
uint64_t mpv_controller_get_pts_nanos(mpv_controller_t *ctrl);

/**
 * Get current playback position in seconds.
 */
double mpv_controller_get_time_pos(mpv_controller_t *ctrl);

/**
 * Render the current video frame into the active OpenGL viewport / FBO.
 */
void mpv_controller_render(mpv_controller_t *ctrl, int width, int height);

/**
 * Poll mpv events.
 */
void mpv_controller_poll_events(mpv_controller_t *ctrl);

/**
 * Check if a new video frame is ready to render.
 */
bool mpv_controller_needs_render(mpv_controller_t *ctrl);

/**
 * Underlying mpv handle.
 */
mpv_handle *mpv_controller_get_handle(mpv_controller_t *ctrl);

#ifdef __cplusplus
}
#endif

#endif /* MPV_CONTROLLER_H */
