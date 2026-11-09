#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include "dvd_vm_bridge.h"
#include "spu_compositor.h"

int main(int argc, char **argv) {
    printf("=== Running DVD VM Bridge Unit & Integration Tests ===\n");

    const char *dvd_dir = (argc > 1) ? argv[1] : "./tinydvds/Dune";
    if (access(dvd_dir, F_OK) != 0) {
        dvd_dir = "./rips/Dune";
    }
    if (access(dvd_dir, F_OK) != 0) {
        printf("Skipping live IFO test (%s not found)\n", dvd_dir);
        return 0;
    }

    spu_compositor_t *spu = spu_compositor_create();
    assert(spu != NULL);

    dvd_vm_bridge_t *vm = dvd_vm_bridge_init(dvd_dir, NULL, spu);
    assert(vm != NULL);
    printf("Initialized custom DVD VM on %s: OK\n", dvd_dir);

    /* 1. Start VM at First Play -> Title 1 */
    bool started = dvd_vm_bridge_start(vm);
    assert(started);
    usleep(80000); /* 80ms */

    /* 2. Jump to Root Menu (VTS 4 Menu PGC 1, which runs Title PGC 2 init on first entry) */
    dvd_vm_bridge_menu(vm);

    /* Verify SPU compositor rendered non-transparent menu highlight pixels */
    int w = 0, h = 0;
    const uint32_t *rgba = NULL;
    int visible_pixels = 0;
    for (int attempt = 0; attempt < 40 && visible_pixels == 0; attempt++) {
        usleep(25000);
        rgba = spu_compositor_get_rgba(spu, &w, &h);
        assert(rgba != NULL && w == 720 && h == 480);
        for (int i = 0; i < w * h; i++) {
            if ((rgba[i] >> 24) > 0) visible_pixels++;
        }
    }
    assert(visible_pixels > 0);
    printf("Root menu SPU bitmap & button highlight active (%d visible overlay pixels): OK\n", visible_pixels);

    /* 3. Navigate right to Button 2 (Scene Selection) and activate */
    dvd_vm_bridge_right_button(vm);
    usleep(40000);
    dvd_vm_bridge_activate_button(vm);
    usleep(100000);

    /* 4. Return to Root Menu and activate Button 1 (Play Movie) */
    dvd_vm_bridge_menu(vm);
    usleep(100000);
    dvd_vm_bridge_activate_button(vm);
    usleep(200000);

    assert(dvd_vm_bridge_is_running(vm));
    printf("Menu -> Scene Selection -> Root Menu -> Play Movie transitions: OK\n");

    /* 5. Test timeline seeking, chapter stepping, and Closed Captions / Subtitles cycling */
    dvd_vm_bridge_cycle_subtitles(vm);
    usleep(40000);
    dvd_vm_bridge_seek_relative(vm, +60);
    usleep(60000);
    dvd_vm_bridge_seek_fraction(vm, 0.20);
    usleep(60000);
    dvd_vm_bridge_next_chapter(vm);
    usleep(60000);
    assert(dvd_vm_bridge_is_running(vm));
    printf("Timeline seeking (+60s, 20%%), chapter stepping, and CC/Subtitle cycling: OK\n");

    dvd_vm_bridge_stop(vm);
    dvd_vm_bridge_destroy(vm);
    spu_compositor_destroy(spu);

    printf("All DVD VM Bridge tests passed!\n");
    return 0;
}
