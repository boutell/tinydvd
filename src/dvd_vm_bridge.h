#ifndef DVD_VM_BRIDGE_H
#define DVD_VM_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include "mpv_controller.h"
#include "spu_compositor.h"

typedef struct dvd_vm_bridge dvd_vm_bridge_t;

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

#ifdef __cplusplus
}
#endif

#endif /* DVD_VM_BRIDGE_H */
