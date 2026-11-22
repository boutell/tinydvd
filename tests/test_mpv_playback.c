/*
 * Desktop playback through mpv, on tests/fixtures/TestDisc.tdvd: the DVD VM
 * drives a real (windowless, silent) mpv, as the desktop player does.
 *
 * Checks that mpv opens the disc's videos inside the package (tdvd://), adds
 * each video's separate audio files as its audio tracks, follows the disc's
 * audio track choice, and plays on the disc's own timeline: the video files
 * carry an edit list for the encoder's frame delay (see tinydvd_mp4.py), so
 * chapter 2 must start at 7 s exactly, not a frame or two later.
 */

#include <mpv/client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "dvd_vm_bridge.h"
#include "mpv_controller.h"
#include "spu_compositor.h"

#define EXPECT(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAILED: " __VA_ARGS__); \
        fprintf(stderr, "\n  (%s:%d)\n", __FILE__, __LINE__); \
        exit(1); \
    } \
} while (0)

static double last_seek = -1;  /* the VM's latest seek target, seconds */

static void on_vm_event(void *user_data, const dvd_vm_event_t *ev) {
    (void)user_data;
    if (ev->type == DVD_VM_EVENT_SEEK) last_seek = (double)ev->pts_nanos / 1e9;
}

static mpv_controller_t *ctrl;
static mpv_handle *mpv;
static mpv_handle *watch;  /* our own client of the same mpv, for its events */

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Run mpv's event loop (as the app's main loop does) for a while */
static void pump(double seconds) {
    double end = now_sec() + seconds;
    while (now_sec() < end) {
        mpv_controller_poll_events(ctrl);
        usleep(10000);
    }
}

static char *prop_str(const char *name) {
    return mpv_get_property_string(mpv, name);
}

static int64_t prop_int(const char *name, int64_t fallback) {
    int64_t v;
    return mpv_get_property(mpv, name, MPV_FORMAT_INT64, &v) == 0 ? v : fallback;
}

static double prop_double(const char *name, double fallback) {
    double v;
    return mpv_get_property(mpv, name, MPV_FORMAT_DOUBLE, &v) == 0 ? v : fallback;
}

/* Where playback lands after a jump: the frame on screen just after mpv's
 * last playback restart (a load or seek finishing) within `seconds`. The
 * test runs mpv at 1% speed, so that frame stays on screen for seconds and
 * this can't catch a later one. -1 if none */
static double landing_position(double seconds) {
    double restart_at = -1, end = now_sec() + seconds;
    while (now_sec() < end) {
        mpv_controller_poll_events(ctrl);
        mpv_event *ev = mpv_wait_event(watch, 0.005);
        if (ev->event_id == MPV_EVENT_PLAYBACK_RESTART) restart_at = now_sec();
    }
    if (restart_at < 0) return -1;
    double t;
    return mpv_get_property(watch, "time-pos", MPV_FORMAT_DOUBLE, &t) == 0 ? t : -1;
}

/* Wait (pumping events) until the loaded file's path ends with suffix */
static int wait_for_path(const char *suffix, double timeout) {
    double end = now_sec() + timeout;
    while (now_sec() < end) {
        mpv_controller_poll_events(ctrl);
        char *path = prop_str("path");
        int ok = path && strlen(path) >= strlen(suffix) &&
                 strcmp(path + strlen(path) - strlen(suffix), suffix) == 0 &&
                 prop_int("track-list/count", 0) > 0;
        mpv_free(path);
        if (ok) return 1;
        usleep(10000);
    }
    return 0;
}

static int count_external_audio(void) {
    int64_t n = prop_int("track-list/count", 0);
    int found = 0;
    for (int64_t i = 0; i < n; i++) {
        char key[64];
        snprintf(key, sizeof(key), "track-list/%lld/type", (long long)i);
        char *type = prop_str(key);
        snprintf(key, sizeof(key), "track-list/%lld/external", (long long)i);
        int ext = 0;
        mpv_get_property(mpv, key, MPV_FORMAT_FLAG, &ext);
        if (type && strcmp(type, "audio") == 0 && ext) found++;
        mpv_free(type);
    }
    return found;
}

int main(int argc, char **argv) {
    const char *disc = (argc > 1) ? argv[1] : "tests/fixtures/TestDisc.tdvd";
    printf("=== Desktop (mpv) playback tests on %s ===\n", disc);

    ctrl = mpv_controller_init(NULL, NULL, NULL);
    EXPECT(ctrl, "mpv_controller_init");
    mpv = mpv_controller_get_handle(ctrl);
    mpv_set_property_string(mpv, "ao", "null");  /* silent, and no audio device needed */
    watch = mpv_create_client(mpv, "test");
    EXPECT(watch, "mpv_create_client");
    /* Slow motion: each frame stays up ~3 s, so landing_position() reads the
     * frame a jump landed on, not one shortly after */
    mpv_set_property_string(mpv, "speed", "0.01");

    spu_compositor_t *spu = spu_compositor_create();
    dvd_vm_bridge_t *vm = dvd_vm_bridge_init(disc, mpv_controller_get_adapter(ctrl), spu);
    EXPECT(vm, "couldn't open %s", disc);
    dvd_vm_bridge_set_event_callback(vm, on_vm_event, NULL);
    EXPECT(dvd_vm_bridge_start(vm), "dvd_vm_bridge_start");

    /* First play: the title, read from inside the package, with its 2 audio files */
    EXPECT(wait_for_path("TestDisc.tdvd", 10.0), "mpv never loaded a video from the package");
    char *path = prop_str("path");
    EXPECT(path && strncmp(path, "tdvd://", 7) == 0, "expected a tdvd:// path, got %s", path ? path : "(none)");
    mpv_free(path);
    pump(1.0);
    EXPECT(count_external_audio() == 2, "expected 2 audio files as tracks, found %d", count_external_audio());
    char *title_path = prop_str("path");
    printf("Title loads from the package with its 2 audio files as tracks: OK\n");

    /* The menu, then chapter 2 (7 s) via button 2 */
    dvd_vm_bridge_menu(vm);
    pump(1.5);
    char *menu_path = prop_str("path");
    EXPECT(menu_path && strcmp(menu_path, title_path) != 0, "the menu didn't load its own video");
    EXPECT(count_external_audio() == 1, "menu: expected 1 audio file, found %d", count_external_audio());
    mpv_free(menu_path);
    dvd_vm_bridge_right_button(vm);
    pump(0.3);
    dvd_vm_bridge_activate_button(vm);
    /* Chapter 2 starts where dvdauthor put it, at a group of pictures near
     * 7 s; the VM seeks to that frame's exact time on the DVD's clock. mpv
     * must land on that very frame: a frame or two later (33-67 ms) would
     * mean a frame delay the edit list didn't hide */
    double t = landing_position(2.0);
    EXPECT(last_seek > 6.0 && last_seek < 8.0, "chapter 2: expected a seek near 7 s, got %.3f", last_seek);
    EXPECT(t > last_seek - 0.002 && t < last_seek + 0.002,
           "chapter 2: the VM seeked to %.3f s but mpv landed on the frame at %.3f s", last_seek, t);
    printf("Menu, then chapter 2 lands on its first frame (%.3f s): OK\n", t);

    /* Back to the start: the edit list puts the first frame at 0, not 0.067 */
    dvd_vm_bridge_seek_fraction(vm, 0.0);
    double first = landing_position(1.5);
    EXPECT(first >= 0 && first < 0.02, "start: expected the first frame at 0, it landed at %.3f", first);
    printf("First frame at %.3f s (the encoder's frame delay is hidden): OK\n", first);

    /* The disc's audio track choice reaches mpv */
    dvd_vm_bridge_cycle_audio(vm);
    pump(1.0);
    EXPECT(prop_int("aid", -1) == 2, "after cycling audio, expected aid 2, have %lld", (long long)prop_int("aid", -1));
    dvd_vm_bridge_cycle_audio(vm);
    pump(1.0);
    EXPECT(prop_int("aid", -1) == 1, "after cycling again, expected aid 1, have %lld", (long long)prop_int("aid", -1));
    printf("Audio track changes reach mpv: OK\n");

    mpv_free(title_path);
    dvd_vm_bridge_stop(vm);
    dvd_vm_bridge_destroy(vm);
    spu_compositor_destroy(spu);
    mpv_destroy(watch);
    mpv_controller_destroy(ctrl);
    printf("All desktop playback tests passed!\n");
    return 0;
}
