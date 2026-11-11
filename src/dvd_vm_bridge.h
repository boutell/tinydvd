#ifndef DVD_VM_BRIDGE_H
#define DVD_VM_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include "mpv_controller.h"
#include "spu_compositor.h"

typedef struct dvd_vm_bridge dvd_vm_bridge_t;

typedef enum {
    DVD_VM_EVENT_NONE = 0,
    DVD_VM_EVENT_LOAD_VIDEO,    /* video_name, pts_nanos */
    DVD_VM_EVENT_SEEK,          /* pts_nanos */
    DVD_VM_EVENT_PAUSE,         /* paused */
    DVD_VM_EVENT_HIGHLIGHT,     /* highlight_data_url, frame_w, frame_h, active_button, buttons */
    DVD_VM_EVENT_OSD,           /* osd_message */
    DVD_VM_EVENT_STATE          /* domain, vts_nr, pgc_nr, cell_nr, chapter_nr */
} dvd_vm_event_type_t;

typedef struct {
    int btn_id;
    int x1, y1, x2, y2;
} dvd_vm_button_rect_t;

typedef struct {
    dvd_vm_event_type_t type;
    const char *video_name;
    int64_t pts_nanos;
    bool paused;
    char *highlight_data_url;    /* Heap-allocated or transient Base64 PNG data URL */
    int frame_w;
    int frame_h;
    int active_button;           /* 1-based, or 0 if none */
    int button_count;
    dvd_vm_button_rect_t buttons[36];
    const char *osd_message;
    int domain;
    int vts_nr;
    int pgc_nr;
    int cell_nr;
    int chapter_nr;
} dvd_vm_event_t;

typedef void (*dvd_vm_event_callback_t)(void *user_data, const dvd_vm_event_t *event);

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize dvd_vm_bridge.
 * @param dvd_path Path to the DVD root directory containing .mvob/.VOB and .IFO files.
 * @param mpv Pointer to initialized mpv_controller.
 * @param spu Pointer to initialized spu_compositor.
 */
dvd_vm_bridge_t *dvd_vm_bridge_init(const char *dvd_path, mpv_controller_t *mpv, spu_compositor_t *spu);

/**
 * Start the background DVD navigation event pump thread.
 */
bool dvd_vm_bridge_start(dvd_vm_bridge_t *bridge);

/**
 * Stop and join the background thread.
 */
void dvd_vm_bridge_stop(dvd_vm_bridge_t *bridge);

/**
 * Free all bridge resources.
 */
void dvd_vm_bridge_destroy(dvd_vm_bridge_t *bridge);

/**
 * User input handlers. Coordinates (x, y) are DVD pixel coordinates: 720 wide,
 * and 480 (NTSC) or 576 (PAL) high, see spu_compositor_get_frame_height().
 */
void dvd_vm_bridge_mouse_move(dvd_vm_bridge_t *bridge, int x, int y);
void dvd_vm_bridge_mouse_click(dvd_vm_bridge_t *bridge, int x, int y);
void dvd_vm_bridge_up_button(dvd_vm_bridge_t *bridge);
void dvd_vm_bridge_down_button(dvd_vm_bridge_t *bridge);
void dvd_vm_bridge_left_button(dvd_vm_bridge_t *bridge);
void dvd_vm_bridge_right_button(dvd_vm_bridge_t *bridge);
void dvd_vm_bridge_activate_button(dvd_vm_bridge_t *bridge);
void dvd_vm_bridge_menu(dvd_vm_bridge_t *bridge);

/**
 * Handle Escape key: returns up a menu level if possible.
 * Returns true if navigated up a menu level; returns false if already at top menu level (meaning exit).
 */
bool dvd_vm_bridge_escape(dvd_vm_bridge_t *bridge);

/**
 * Timeline navigation, pause, and Closed Caption / Subtitle / Audio track controls.
 */
void dvd_vm_bridge_toggle_pause(dvd_vm_bridge_t *bridge);
void dvd_vm_bridge_seek_relative(dvd_vm_bridge_t *bridge, int delta_seconds);
void dvd_vm_bridge_seek_fraction(dvd_vm_bridge_t *bridge, double fraction_0_to_1);
void dvd_vm_bridge_next_chapter(dvd_vm_bridge_t *bridge);
void dvd_vm_bridge_prev_chapter(dvd_vm_bridge_t *bridge);
void dvd_vm_bridge_cycle_subtitles(dvd_vm_bridge_t *bridge);
void dvd_vm_bridge_cycle_audio(dvd_vm_bridge_t *bridge);

/**
 * Check if the DVD VM is running or reached stop.
 */
bool dvd_vm_bridge_is_running(dvd_vm_bridge_t *bridge);

/**
 * Register an event callback for web streaming or GUI event notification.
 */
void dvd_vm_bridge_set_event_callback(dvd_vm_bridge_t *bridge, dvd_vm_event_callback_t cb, void *user_data);

/**
 * Report current playback PTS from an external player (e.g. HTML5 video client).
 */
void dvd_vm_bridge_report_pts(dvd_vm_bridge_t *bridge, int64_t pts_nanos);

/**
 * Get the underlying SPU compositor.
 */
spu_compositor_t *dvd_vm_bridge_get_spu(dvd_vm_bridge_t *bridge);

/**
 * Get current loaded companion video filename (e.g. "VTS_01_VIDEO.mkv").
 */
const char *dvd_vm_bridge_get_loaded_video(dvd_vm_bridge_t *bridge);

/**
 * Get current playback PTS in nanoseconds.
 */
int64_t dvd_vm_bridge_get_current_pts_nanos(dvd_vm_bridge_t *bridge);

/**
 * Check if playback is currently paused.
 */
bool dvd_vm_bridge_is_paused(dvd_vm_bridge_t *bridge);

/**
 * Set playback pause state (e.g. from web client play/pause state).
 */
void dvd_vm_bridge_set_paused(dvd_vm_bridge_t *bridge, bool paused);

#ifdef __cplusplus
}
#endif

#endif /* DVD_VM_BRIDGE_H */
