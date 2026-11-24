/*
 * DVD VM tests, run against tests/fixtures/TestDisc.tdvd, a small synthetic
 * disc (see tests/fixtures/make-test-disc.sh for what's on it).
 *
 * The VM runs without a player here, so the test follows it through the
 * events it reports: which video it loads (the menu's or the title's),
 * seeks, the active menu button, and on-screen messages.
 */

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "dvd_vm_bridge.h"
#include "spu_compositor.h"

#define MENU_VIDEO "VTS_01_0_VIDEO.mp4"
#define TITLE_VIDEO "VTS_01_VIDEO.mp4"
#define SEC 1000000000LL

typedef struct {
    pthread_mutex_t lock;
    char video[64];
    int loads;
    int64_t seek_pts;
    int seeks;
    int active_button;
    int button_count;
    char osd[256];
    int osds;
} observed_t;

static observed_t obs = { .lock = PTHREAD_MUTEX_INITIALIZER, .seek_pts = -1 };

static void on_event(void *user_data, const dvd_vm_event_t *ev) {
    (void)user_data;
    pthread_mutex_lock(&obs.lock);
    switch (ev->type) {
    case DVD_VM_EVENT_LOAD_VIDEO:
        snprintf(obs.video, sizeof(obs.video), "%s", ev->video_name ? ev->video_name : "");
        obs.loads++;
        break;
    case DVD_VM_EVENT_SEEK:
        obs.seek_pts = ev->pts_nanos;
        obs.seeks++;
        break;
    case DVD_VM_EVENT_HIGHLIGHT:
        obs.active_button = ev->active_button;
        obs.button_count = ev->button_count;
        break;
    case DVD_VM_EVENT_OSD:
        snprintf(obs.osd, sizeof(obs.osd), "%s", ev->osd_message ? ev->osd_message : "");
        obs.osds++;
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&obs.lock);
}

static observed_t snapshot(void) {
    pthread_mutex_lock(&obs.lock);
    observed_t s = obs;
    pthread_mutex_unlock(&obs.lock);
    return s;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Poll until cond(snapshot) holds, up to 3 seconds */
static int wait_until(int (*cond)(const observed_t *, const void *), const void *arg) {
    double deadline = now_sec() + 3.0;
    while (now_sec() < deadline) {
        observed_t s = snapshot();
        if (cond(&s, arg)) return 1;
        usleep(10000);
    }
    return 0;
}

static int playing(const observed_t *s, const void *name) {
    return strcmp(s->video, (const char *)name) == 0;
}
static int button_is(const observed_t *s, const void *n) {
    return s->active_button == *(const int *)n;
}
static int seeked_since(const observed_t *s, const void *n) {
    return s->seeks > *(const int *)n;
}
static int osd_since(const observed_t *s, const void *n) {
    return s->osds > *(const int *)n;
}
static int loaded_since(const observed_t *s, const void *n) {
    return s->loads > *(const int *)n;
}

#define EXPECT(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAILED: " __VA_ARGS__); \
        fprintf(stderr, "\n  (%s:%d)\n", __FILE__, __LINE__); \
        exit(1); \
    } \
} while (0)

static void expect_playing(const char *name, const char *step) {
    EXPECT(wait_until(playing, name), "%s: expected %s to be loaded, have \"%s\"",
           step, name, snapshot().video);
}

/* Seek to about `sec` seconds into the title, reported via a SEEK event or
 * the playback position */
static void expect_position(double sec, int seeks_before, const char *step) {
    wait_until(seeked_since, &seeks_before);
    usleep(50000);
    observed_t s = snapshot();
    int64_t pts = (s.seeks > seeks_before) ? s.seek_pts : -1;
    double got = pts >= 0 ? (double)pts / SEC : -1;
    EXPECT(got >= sec - 1.0 && got <= sec + 1.0,
           "%s: expected position near %.1fs, got %.2fs", step, sec, got);
}

/* Counts visible overlay pixels inside a button's rectangle */
static int visible_in(spu_compositor_t *spu, int x0, int y0, int x1, int y1) {
    int w = 0, h = 0;
    const uint32_t *rgba = spu_compositor_get_rgba(spu, &w, &h);
    if (!rgba) return 0;
    int n = 0;
    for (int y = y0; y < y1 && y < h; y++)
        for (int x = x0; x < x1 && x < w; x++)
            if ((rgba[y * w + x] >> 24) > 0) n++;
    return n;
}

int main(int argc, char **argv) {
    const char *disc = (argc > 1) ? argv[1] : "tests/fixtures/TestDisc.tdvd";
    printf("=== DVD VM tests on %s ===\n", disc);

    spu_compositor_t *spu = spu_compositor_create();
    EXPECT(spu, "spu_compositor_create");
    dvd_vm_bridge_t *vm = dvd_vm_bridge_init(disc, NULL, spu);
    EXPECT(vm, "couldn't open %s", disc);
    dvd_vm_bridge_set_event_callback(vm, on_event, NULL);

    /* First play jumps straight to title 1 */
    EXPECT(dvd_vm_bridge_start(vm), "dvd_vm_bridge_start");
    expect_playing(TITLE_VIDEO, "first play");
    printf("First play starts title 1: OK\n");

    /* The menu key opens the root menu, with its two buttons */
    dvd_vm_bridge_menu(vm);
    expect_playing(MENU_VIDEO, "menu key");
    int one = 1, two = 2;
    EXPECT(wait_until(button_is, &one), "root menu: expected button 1 active, have %d",
           snapshot().active_button);
    EXPECT(snapshot().button_count == 2, "root menu: expected 2 buttons, have %d",
           snapshot().button_count);
    usleep(100000);
    int w = 0, h = 0;
    EXPECT(spu_compositor_get_rgba(spu, &w, &h) && w == 720 && h == 480,
           "menu overlay: expected 720x480, have %dx%d", w, h);
    /* Button rectangles from make-test-disc.sh */
    EXPECT(visible_in(spu, 100, 360, 300, 420) > 0, "menu overlay: button 1 isn't drawn");
    EXPECT(visible_in(spu, 420, 360, 620, 420) > 0, "menu overlay: button 2 isn't drawn");
    EXPECT(visible_in(spu, 0, 0, 720, 300) == 0, "menu overlay: drawn where there are no buttons");
    /* Unselected buttons are outlines; the active one is filled in */
    EXPECT(visible_in(spu, 100, 360, 300, 420) > visible_in(spu, 420, 360, 620, 420),
           "menu overlay: button 1 isn't highlighted");
    printf("Root menu shows its 2 buttons, button 1 highlighted: OK\n");

    /* Right moves to button 2; activating it plays chapter 2 (7 s in) */
    dvd_vm_bridge_right_button(vm);
    EXPECT(wait_until(button_is, &two), "right: expected button 2 active, have %d",
           snapshot().active_button);
    usleep(50000);
    EXPECT(visible_in(spu, 420, 360, 620, 420) > visible_in(spu, 100, 360, 300, 420),
           "menu overlay: highlight didn't move to button 2");
    int loads = snapshot().loads, seeks = snapshot().seeks;
    dvd_vm_bridge_activate_button(vm);
    EXPECT(wait_until(loaded_since, &loads), "activate button 2: nothing loaded");
    expect_playing(TITLE_VIDEO, "activate button 2");
    expect_position(7.0, seeks, "activate button 2 (chapter 2)");
    printf("Menu button 2 plays chapter 2: OK\n");

    /* Back to the menu, left to button 1, which plays from the start */
    dvd_vm_bridge_menu(vm);
    expect_playing(MENU_VIDEO, "menu key from title");
    dvd_vm_bridge_left_button(vm);
    EXPECT(wait_until(button_is, &one), "left: expected button 1 active, have %d",
           snapshot().active_button);
    seeks = snapshot().seeks;
    dvd_vm_bridge_activate_button(vm);
    expect_playing(TITLE_VIDEO, "activate button 1");
    expect_position(0.0, seeks, "activate button 1");
    printf("Menu button 1 plays from the start: OK\n");

    /* Chapter skipping: chapters start at 0, 7 and 14 s */
    seeks = snapshot().seeks;
    dvd_vm_bridge_next_chapter(vm);
    expect_position(7.0, seeks, "next chapter");
    seeks = snapshot().seeks;
    dvd_vm_bridge_next_chapter(vm);
    expect_position(14.0, seeks, "next chapter again");
    printf("Chapter skipping: OK\n");

    /* Seeking: halfway through the 20 s title, then back 3 s */
    seeks = snapshot().seeks;
    dvd_vm_bridge_seek_fraction(vm, 0.5);
    expect_position(10.0, seeks, "seek to 50%");
    seeks = snapshot().seeks;
    dvd_vm_bridge_seek_relative(vm, -3);
    expect_position(7.0, seeks, "seek back 3 s");
    /* The timeline: to a time in the video, which must stay in the title */
    seeks = snapshot().seeks;
    dvd_vm_bridge_seek_absolute(vm, 16.0);
    expect_position(16.0, seeks, "seek to 16 s");
    seeks = snapshot().seeks;
    dvd_vm_bridge_seek_absolute(vm, 4.0);
    expect_position(4.0, seeks, "seek back to 4 s");
    seeks = snapshot().seeks;
    dvd_vm_bridge_seek_absolute(vm, 500.0);
    expect_position(19.5, seeks, "seek past the end of the title");
    printf("Seeking: OK\n");

    /* Subtitles: the disc has one track, and cycling reports it */
    int osds = snapshot().osds;
    dvd_vm_bridge_cycle_subtitles(vm);
    EXPECT(wait_until(osd_since, &osds), "cycle subtitles: no message shown");
    printf("Subtitle cycling (\"%s\"): OK\n", snapshot().osd);

    EXPECT(dvd_vm_bridge_is_running(vm), "VM stopped unexpectedly");
    dvd_vm_bridge_stop(vm);
    dvd_vm_bridge_destroy(vm);
    spu_compositor_destroy(spu);
    printf("All DVD VM tests passed!\n");
    return 0;
}
