#include "native_ui.h"

#include <cairo.h>
#include <ctype.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

extern bool native_dialog_choose_source(bool choose_iso_only, const char *initial_path,
                                        char *out_path, size_t out_len);

#ifndef __APPLE__
static bool run_linux_dialog_cmd(char *const argv[], char *out_path, size_t out_len) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        if (pipefd[1] > STDERR_FILENO) close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(pipefd[1]);
    size_t total = 0;
    ssize_t n;
    while (total + 1 < out_len &&
           (n = read(pipefd[0], out_path + total, out_len - 1 - total)) > 0) {
        total += (size_t)n;
    }
    out_path[total] = '\0';
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    while (total > 0 && (out_path[total - 1] == '\n' || out_path[total - 1] == '\r')) {
        out_path[--total] = '\0';
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && total > 0;
}

bool native_dialog_choose_source(bool choose_iso_only, const char *initial_path,
                                 char *out_path, size_t out_len) {
    if (!out_path || out_len == 0) return false;
    out_path[0] = '\0';
    const char *start_dir = (initial_path && initial_path[0] && strncmp(initial_path, "/dev/", 5) != 0)
                            ? initial_path : ".";
    if (choose_iso_only) {
        char *z_argv[] = {
            "zenity", "--file-selection",
            "--title=Select DVD ISO Image",
            "--file-filter=DVD Disc Images | *.iso *.ISO *.img *.IMG",
            NULL
        };
        if (run_linux_dialog_cmd(z_argv, out_path, out_len)) return true;
        char *k_argv[] = { "kdialog", "--getopenfilename", (char *)start_dir, "*.iso *.img|DVD Images", NULL };
        return run_linux_dialog_cmd(k_argv, out_path, out_len);
    } else {
        char *z_argv[] = {
            "zenity", "--file-selection", "--directory",
            "--title=Select DVD Folder (VIDEO_TS or Rip)",
            NULL
        };
        if (run_linux_dialog_cmd(z_argv, out_path, out_len)) return true;
        char *k_argv[] = { "kdialog", "--getexistingdirectory", (char *)start_dir, NULL };
        return run_linux_dialog_cmd(k_argv, out_path, out_len);
    }
}
#endif

typedef struct {
    double x, y, w, h;
} ui_rect_t;

static bool pt_in_rect(double px, double py, ui_rect_t r) {
    return r.w > 0 && r.h > 0 && px >= r.x && px <= (r.x + r.w) && py >= r.y && py <= (r.y + r.h);
}

typedef enum {
    FOCUS_NONE = 0,
    FOCUS_SEARCH,
    FOCUS_MODAL_SOURCE,
    FOCUS_MODAL_TITLE
} ui_focus_t;

typedef struct {
    char text[512];
    int len;
    int cursor;
    bool select_all;
} ui_textfield_t;

typedef struct {
    char id[128];
    cairo_surface_t *surface;
    bool attempted;
} ui_thumb_entry_t;

typedef struct {
    ui_rect_t rect;
    int catalog_index;
} ui_card_hit_t;

#define MAX_THUMB_CACHE 256

struct native_ui {
    SDL_Window *window;
    tdvd_library_t *lib;

    GLuint gl_tex;
    bool gl_initialized;
    uint8_t *pixel_buf;
    int tex_w;
    int tex_h;

    bool dirty;
    int last_win_w;
    int last_win_h;
    bool last_player_view;
    bool last_hud_visible;
    uint32_t last_tick_ms;

    ui_focus_t focus;
    ui_textfield_t tf_search;
    ui_textfield_t tf_source;
    ui_textfield_t tf_title;
    bool enrich_checked;

    /* Filtered catalog snapshot */
    catalog_item_t *items_snapshot;
    int snapshot_count;
    int *filtered_indices;
    int filtered_count;
    int selected_filtered;
    double scroll_y;
    double max_scroll_y;
    int hovered_card;
    int mouse_x;
    int mouse_y;

    /* Modal state */
    bool import_modal_open;
    bool import_prefilled;
    import_state_t last_import_state;
    char local_import_err[256];
    double log_scroll_offset;

    /* Hit boxes in logical window coords */
    ui_rect_t r_search;
    ui_rect_t r_search_clear;
    ui_rect_t r_open_import;
    ui_rect_t r_empty_import;
    ui_rect_t r_back_lib;

    ui_rect_t r_modal_box;
    ui_rect_t r_modal_close;
    ui_rect_t r_modal_source;
    ui_rect_t r_modal_browse;
    ui_rect_t r_modal_browse_iso;
    ui_rect_t r_modal_title;
    ui_rect_t r_modal_enrich;
    ui_rect_t r_modal_start;
    ui_rect_t r_modal_cancel;
    ui_rect_t r_modal_done;
    ui_rect_t r_modal_play;

    ui_card_hit_t *card_hits;
    int card_hit_count;
    int card_hit_cap;

    ui_thumb_entry_t thumbs[MAX_THUMB_CACHE];
    int thumb_count;
};

/* =========================================================================
 * UTF-8 Text Field Helpers
 * ========================================================================= */

static void tf_set(ui_textfield_t *tf, const char *s) {
    if (!tf) return;
    snprintf(tf->text, sizeof(tf->text), "%s", s ? s : "");
    tf->len = (int)strlen(tf->text);
    tf->cursor = tf->len;
    tf->select_all = false;
}

static void tf_insert_str(ui_textfield_t *tf, const char *ins) {
    if (!tf || !ins || !ins[0]) return;
    if (tf->select_all) {
        tf->text[0] = '\0';
        tf->len = 0;
        tf->cursor = 0;
        tf->select_all = false;
    }
    int ilen = (int)strlen(ins);
    if (tf->len + ilen >= (int)sizeof(tf->text)) {
        ilen = (int)sizeof(tf->text) - 1 - tf->len;
    }
    if (ilen <= 0) return;
    memmove(tf->text + tf->cursor + ilen, tf->text + tf->cursor, (size_t)(tf->len - tf->cursor + 1));
    memcpy(tf->text + tf->cursor, ins, (size_t)ilen);
    tf->len += ilen;
    tf->cursor += ilen;
}

static void tf_backspace(ui_textfield_t *tf) {
    if (!tf || tf->len == 0) return;
    if (tf->select_all) {
        tf_set(tf, "");
        return;
    }
    if (tf->cursor <= 0) return;
    int prev = tf->cursor - 1;
    while (prev > 0 && (((unsigned char)tf->text[prev] & 0xC0) == 0x80)) {
        prev--;
    }
    int del = tf->cursor - prev;
    memmove(tf->text + prev, tf->text + tf->cursor, (size_t)(tf->len - tf->cursor + 1));
    tf->len -= del;
    tf->cursor = prev;
}

static void tf_delete_fwd(ui_textfield_t *tf) {
    if (!tf || tf->len == 0) return;
    if (tf->select_all) {
        tf_set(tf, "");
        return;
    }
    if (tf->cursor >= tf->len) return;
    int next = tf->cursor + 1;
    while (next < tf->len && (((unsigned char)tf->text[next] & 0xC0) == 0x80)) {
        next++;
    }
    int del = next - tf->cursor;
    memmove(tf->text + tf->cursor, tf->text + next, (size_t)(tf->len - next + 1));
    tf->len -= del;
}

static void tf_move_left(ui_textfield_t *tf) {
    if (!tf) return;
    tf->select_all = false;
    if (tf->cursor <= 0) return;
    tf->cursor--;
    while (tf->cursor > 0 && (((unsigned char)tf->text[tf->cursor] & 0xC0) == 0x80)) {
        tf->cursor--;
    }
}

static void tf_move_right(ui_textfield_t *tf) {
    if (!tf) return;
    tf->select_all = false;
    if (tf->cursor >= tf->len) return;
    tf->cursor++;
    while (tf->cursor < tf->len && (((unsigned char)tf->text[tf->cursor] & 0xC0) == 0x80)) {
        tf->cursor++;
    }
}

/* =========================================================================
 * Cairo / PNG / Typography Helpers
 * ========================================================================= */

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t offset;
} png_mem_reader_t;

static cairo_status_t png_read_from_mem(void *closure, unsigned char *data, unsigned int length) {
    png_mem_reader_t *r = (png_mem_reader_t *)closure;
    if (r->offset + length > r->size) {
        return CAIRO_STATUS_READ_ERROR;
    }
    memcpy(data, r->data + r->offset, length);
    r->offset += length;
    return CAIRO_STATUS_SUCCESS;
}

static cairo_surface_t *ui_get_thumbnail_surface(native_ui_t *ui, const catalog_item_t *item) {
    if (!ui || !item || !item->has_thumbnail) return NULL;
    for (int i = 0; i < ui->thumb_count; i++) {
        if (strcmp(ui->thumbs[i].id, item->id) == 0) {
            return ui->thumbs[i].surface;
        }
    }
    if (ui->thumb_count >= MAX_THUMB_CACHE) return NULL;

    ui_thumb_entry_t *entry = &ui->thumbs[ui->thumb_count++];
    snprintf(entry->id, sizeof(entry->id), "%s", item->id);
    entry->attempted = true;
    entry->surface = NULL;

    size_t sz = 0;
    uint8_t *png = tdvd_library_read_thumbnail(item->path, &sz);
    if (png && sz > 0) {
        png_mem_reader_t reader = { .data = png, .size = sz, .offset = 0 };
        cairo_surface_t *s = cairo_image_surface_create_from_png_stream(png_read_from_mem, &reader);
        if (s && cairo_surface_status(s) == CAIRO_STATUS_SUCCESS) {
            entry->surface = s;
        } else if (s) {
            cairo_surface_destroy(s);
        }
        free(png);
    }
    return entry->surface;
}

static void cairo_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (r > w * 0.5) r = w * 0.5;
    if (r > h * 0.5) r = h * 0.5;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI * 0.5, 0.0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0, M_PI * 0.5);
    cairo_arc(cr, x + r, y + h - r, r, M_PI * 0.5, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, M_PI * 1.5);
    cairo_close_path(cr);
}

static void cairo_top_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (r > w * 0.5) r = w * 0.5;
    if (r > h * 0.5) r = h * 0.5;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI * 0.5, 0.0);
    cairo_line_to(cr, x + w, y + h);
    cairo_line_to(cr, x, y + h);
    cairo_arc(cr, x + r, y + r, r, M_PI, M_PI * 1.5);
    cairo_close_path(cr);
}

static void draw_text_box(cairo_t *cr, const char *text, double x, double y, double w,
                          int max_lines, double font_px, bool bold, bool monospace,
                          double r, double g, double b, double a,
                          PangoAlignment align, double *out_h) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, monospace ? "Monospace" : "Sans");
    pango_font_description_set_absolute_size(desc, font_px * PANGO_SCALE);
    pango_font_description_set_weight(desc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    pango_layout_set_text(layout, text ? text : "", -1);
    if (w > 0) {
        pango_layout_set_width(layout, (int)(w * PANGO_SCALE));
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        if (max_lines > 0) {
            pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
            pango_layout_set_height(layout, -max_lines);
        }
    }
    pango_layout_set_alignment(layout, align);

    if (out_h) {
        int lw = 0, lh = 0;
        pango_layout_get_pixel_size(layout, &lw, &lh);
        *out_h = (double)lh;
    }

    cairo_save(cr);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);

    g_object_unref(layout);
}

static double measure_text_width(cairo_t *cr, const char *text, double font_px, bool bold, bool monospace) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, monospace ? "Monospace" : "Sans");
    pango_font_description_set_absolute_size(desc, font_px * PANGO_SCALE);
    pango_font_description_set_weight(desc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    pango_layout_set_text(layout, text ? text : "", -1);
    int lw = 0, lh = 0;
    pango_layout_get_pixel_size(layout, &lw, &lh);
    g_object_unref(layout);
    return (double)lw;
}

static void draw_textfield(cairo_t *cr, ui_rect_t r, const ui_textfield_t *tf,
                           const char *placeholder, bool focused, bool rounded_pill,
                           double left_pad, double right_pad) {
    cairo_save(cr);
    cairo_rounded_rect(cr, r.x, r.y, r.w, r.h, rounded_pill ? (r.h * 0.5) : 8.0);
    cairo_set_source_rgba(cr, 0.043, 0.047, 0.063, 1.0);
    cairo_fill_preserve(cr);
    if (focused) {
        cairo_set_source_rgba(cr, 0.40, 0.99, 0.95, 0.95);
        cairo_set_line_width(cr, 1.6);
    } else {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.18);
        cairo_set_line_width(cr, 1.0);
    }
    cairo_stroke(cr);

    double inner_x = r.x + left_pad;
    double inner_w = r.w - left_pad - right_pad;
    double text_y = r.y + (r.h - 16.0) * 0.5;

    cairo_rounded_rect(cr, inner_x, r.y + 2, inner_w > 4 ? inner_w : 4, r.h - 4, 4.0);
    cairo_clip(cr);

    if (tf->len == 0) {
        draw_text_box(cr, placeholder, inner_x, text_y, inner_w, 1, 13.5, false, false,
                      0.53, 0.57, 0.69, 0.75, PANGO_ALIGN_LEFT, NULL);
        if (focused) {
            cairo_set_source_rgba(cr, 0.40, 0.99, 0.95, 1.0);
            cairo_rectangle(cr, inner_x + 1.0, r.y + 8.0, 1.5, r.h - 16.0);
            cairo_fill(cr);
        }
    } else {
        /* Compute pixel X up to cursor and scroll horizontally if overflowing */
        char prefix[512];
        int clen = tf->cursor;
        if (clen > (int)sizeof(prefix) - 1) clen = (int)sizeof(prefix) - 1;
        memcpy(prefix, tf->text, (size_t)clen);
        prefix[clen] = '\0';

        double cur_px = measure_text_width(cr, prefix, 13.5, false, false);
        double scroll_x = 0.0;
        if (cur_px > inner_w - 6.0) {
            scroll_x = cur_px - (inner_w - 6.0);
        }

        if (focused && tf->select_all) {
            double full_w = measure_text_width(cr, tf->text, 13.5, false, false);
            double sel_w = fmin(full_w, inner_w);
            cairo_set_source_rgba(cr, 0.40, 0.99, 0.95, 0.28);
            cairo_rectangle(cr, inner_x, r.y + 7.0, sel_w, r.h - 14.0);
            cairo_fill(cr);
        }

        draw_text_box(cr, tf->text, inner_x - scroll_x, text_y, 0, 0, 13.5, false, false,
                      1.0, 1.0, 1.0, 0.98, PANGO_ALIGN_LEFT, NULL);

        if (focused && !tf->select_all) {
            cairo_set_source_rgba(cr, 0.40, 0.99, 0.95, 1.0);
            cairo_rectangle(cr, inner_x + cur_px - scroll_x, r.y + 8.0, 1.5, r.h - 16.0);
            cairo_fill(cr);
        }
    }
    cairo_restore(cr);
}

static bool strcasestr_ascii(const char *haystack, const char *needle) {
    if (!needle || !needle[0]) return true;
    if (!haystack || !haystack[0]) return false;
    return strcasestr(haystack, needle) != NULL;
}

static void sync_catalog_snapshot(native_ui_t *ui) {
    if (!ui || !ui->lib) return;

    pthread_mutex_lock(&ui->lib->catalog_lock);
    int n = ui->lib->catalog_count;
    bool changed = (n != ui->snapshot_count);
    if (!changed && n > 0 && ui->items_snapshot) {
        if (memcmp(ui->items_snapshot, ui->lib->catalog, (size_t)n * sizeof(catalog_item_t)) != 0) {
            changed = true;
        }
    }
    if (changed) {
        catalog_item_t *copy = NULL;
        if (n > 0) {
            copy = (catalog_item_t *)malloc((size_t)n * sizeof(catalog_item_t));
            if (copy) {
                memcpy(copy, ui->lib->catalog, (size_t)n * sizeof(catalog_item_t));
            } else {
                n = 0;
            }
        }
        free(ui->items_snapshot);
        ui->items_snapshot = copy;
        ui->snapshot_count = n;
        ui->dirty = true;
    }
    pthread_mutex_unlock(&ui->lib->catalog_lock);

    /* Filter */
    if (ui->snapshot_count <= 0) {
        ui->filtered_count = 0;
        return;
    }

    int *idx = (int *)realloc(ui->filtered_indices, (size_t)ui->snapshot_count * sizeof(int));
    if (!idx) return;
    ui->filtered_indices = idx;

    const char *q = ui->tf_search.text;
    while (*q && isspace((unsigned char)*q)) q++;

    int fc = 0;
    for (int i = 0; i < ui->snapshot_count; i++) {
        const catalog_item_t *it = &ui->items_snapshot[i];
        char year_buf[16] = {0};
        if (it->year > 0) snprintf(year_buf, sizeof(year_buf), "%d", it->year);
        if (!q[0] ||
            strcasestr_ascii(it->title, q) ||
            strcasestr_ascii(it->director, q) ||
            strcasestr_ascii(it->cast, q) ||
            strcasestr_ascii(it->genres, q) ||
            strcasestr_ascii(it->overview, q) ||
            (year_buf[0] && strstr(year_buf, q))) {
            ui->filtered_indices[fc++] = i;
        }
    }
    ui->filtered_count = fc;
    if (ui->selected_filtered >= ui->filtered_count) {
        ui->selected_filtered = ui->filtered_count - 1;
    }
}

/* =========================================================================
 * Modal & Lifecycle
 * ========================================================================= */

static void open_import_modal(native_ui_t *ui) {
    if (!ui || !ui->lib) return;
    ui->import_modal_open = true;
    ui->local_import_err[0] = '\0';

    import_job_t *job = &ui->lib->import_job;
    pthread_mutex_lock(&job->lock);
    bool active = job->active;
    pthread_mutex_unlock(&job->lock);

    if (!active && !ui->import_prefilled) {
        char def_src[512] = {0};
        char def_title[256] = {0};
        tdvd_library_detect_default_source(def_src, sizeof(def_src), def_title, sizeof(def_title));
        if (ui->tf_source.len == 0 && def_src[0]) {
            tf_set(&ui->tf_source, def_src);
        }
        if (ui->tf_title.len == 0 && def_title[0]) {
            tf_set(&ui->tf_title, def_title);
        }
        ui->import_prefilled = true;
    }
    ui->focus = active ? FOCUS_NONE : FOCUS_MODAL_SOURCE;
    ui->dirty = true;
}

static void close_import_modal(native_ui_t *ui) {
    if (!ui) return;
    ui->import_modal_open = false;
    if (ui->focus == FOCUS_MODAL_SOURCE || ui->focus == FOCUS_MODAL_TITLE) {
        ui->focus = FOCUS_NONE;
    }
    ui->dirty = true;
}

static void start_import_from_modal(native_ui_t *ui) {
    if (!ui || !ui->lib) return;
    const char *src = ui->tf_source.text;
    while (*src && isspace((unsigned char)*src)) src++;
    if (!src[0]) {
        snprintf(ui->local_import_err, sizeof(ui->local_import_err),
                 "Please enter a DVD drive device or source path.");
        ui->dirty = true;
        return;
    }
    ui->local_import_err[0] = '\0';
    char err[256] = {0};
    if (!tdvd_library_import_start(ui->lib, src, ui->tf_title.text, !ui->enrich_checked, err, sizeof(err))) {
        snprintf(ui->local_import_err, sizeof(ui->local_import_err), "%s",
                 err[0] ? err : "Failed to start import.");
    } else {
        ui->focus = FOCUS_NONE;
    }
    ui->dirty = true;
}

native_ui_t *native_ui_create(SDL_Window *window, tdvd_library_t *lib) {
    native_ui_t *ui = (native_ui_t *)calloc(1, sizeof(native_ui_t));
    if (!ui) return NULL;
    ui->window = window;
    ui->lib = lib;
    ui->enrich_checked = true;
    ui->hovered_card = -1;
    ui->selected_filtered = -1;
    ui->dirty = true;
    ui->last_import_state = IMPORT_IDLE;
    sync_catalog_snapshot(ui);
    return ui;
}

void native_ui_destroy(native_ui_t *ui) {
    if (!ui) return;
    for (int i = 0; i < ui->thumb_count; i++) {
        if (ui->thumbs[i].surface) {
            cairo_surface_destroy(ui->thumbs[i].surface);
        }
    }
    if (ui->gl_initialized && ui->gl_tex) {
        glDeleteTextures(1, &ui->gl_tex);
    }
    free(ui->pixel_buf);
    free(ui->items_snapshot);
    free(ui->filtered_indices);
    free(ui->card_hits);
    free(ui);
}

bool native_ui_is_modal_open(const native_ui_t *ui) {
    return ui ? ui->import_modal_open : false;
}

void native_ui_mark_dirty(native_ui_t *ui) {
    if (ui) ui->dirty = true;
}

/* =========================================================================
 * Layout & Rendering (Cairo + PangoCairo -> OpenGL 2.1 Texture Quad)
 * ========================================================================= */

static void compute_layout_rects(native_ui_t *ui, int win_w, int win_h, bool in_player_view) {
    memset(&ui->r_search, 0, sizeof(ui->r_search));
    memset(&ui->r_search_clear, 0, sizeof(ui->r_search_clear));
    memset(&ui->r_open_import, 0, sizeof(ui->r_open_import));
    memset(&ui->r_empty_import, 0, sizeof(ui->r_empty_import));
    memset(&ui->r_back_lib, 0, sizeof(ui->r_back_lib));
    memset(&ui->r_modal_box, 0, sizeof(ui->r_modal_box));
    memset(&ui->r_modal_close, 0, sizeof(ui->r_modal_close));
    memset(&ui->r_modal_source, 0, sizeof(ui->r_modal_source));
    memset(&ui->r_modal_browse, 0, sizeof(ui->r_modal_browse));
    memset(&ui->r_modal_browse_iso, 0, sizeof(ui->r_modal_browse_iso));
    memset(&ui->r_modal_title, 0, sizeof(ui->r_modal_title));
    memset(&ui->r_modal_enrich, 0, sizeof(ui->r_modal_enrich));
    memset(&ui->r_modal_start, 0, sizeof(ui->r_modal_start));
    memset(&ui->r_modal_cancel, 0, sizeof(ui->r_modal_cancel));
    memset(&ui->r_modal_done, 0, sizeof(ui->r_modal_done));
    memset(&ui->r_modal_play, 0, sizeof(ui->r_modal_play));
    ui->card_hit_count = 0;

    if (in_player_view) {
        ui->r_back_lib = (ui_rect_t){ 16, 12, 108, 34 };
        ui->r_open_import = (ui_rect_t){ (double)win_w - 154, 12, 138, 34 };
    } else {
        double btn_w = 138;
        ui->r_open_import = (ui_rect_t){ (double)win_w - 24 - btn_w, 18, btn_w, 36 };

        double sw = fmin(380.0, fmax(160.0, (double)win_w - 520.0));
        double sx = ((double)win_w - sw) * 0.5;
        if (sx < 230.0) sx = 230.0;
        if (sx + sw > ui->r_open_import.x - 130.0) {
            sw = fmax(120.0, ui->r_open_import.x - 130.0 - sx);
        }
        ui->r_search = (ui_rect_t){ sx, 18, sw, 36 };
        if (ui->tf_search.len > 0) {
            ui->r_search_clear = (ui_rect_t){ sx + sw - 30, 21, 26, 30 };
        }

        if (ui->snapshot_count == 0) {
            ui->r_empty_import = (ui_rect_t){ ((double)win_w - 160.0) * 0.5, 220.0, 160.0, 38.0 };
        } else if (ui->filtered_count > 0) {
            double pad = 24.0;
            double gap = 20.0;
            double grid_top = 88.0;
            double avail_w = fmax(220.0, (double)win_w - pad * 2.0);
            int cols = (int)((avail_w + gap) / 225.0);
            if (cols < 1) cols = 1;
            double card_w = (avail_w - (cols - 1) * gap) / cols;
            double thumb_h = fmin(card_w * (4.0 / 3.0), 360.0);
            double info_h = 148.0;
            double card_h = thumb_h + info_h;

            int rows = (ui->filtered_count + cols - 1) / cols;
            double total_h = rows * card_h + (rows > 0 ? (rows - 1) * gap : 0.0) + pad;
            double view_h = fmax(100.0, (double)win_h - grid_top);
            ui->max_scroll_y = fmax(0.0, total_h - view_h);
            if (ui->scroll_y < 0.0) ui->scroll_y = 0.0;
            if (ui->scroll_y > ui->max_scroll_y) ui->scroll_y = ui->max_scroll_y;

            if (ui->filtered_count > ui->card_hit_cap) {
                ui->card_hit_cap = ui->filtered_count;
                ui->card_hits = (ui_card_hit_t *)realloc(ui->card_hits, (size_t)ui->card_hit_cap * sizeof(ui_card_hit_t));
            }
            for (int fi = 0; fi < ui->filtered_count; fi++) {
                int r = fi / cols;
                int c = fi % cols;
                double cx = pad + c * (card_w + gap);
                double cy = grid_top + r * (card_h + gap) - ui->scroll_y;
                ui->card_hits[ui->card_hit_count++] = (ui_card_hit_t){
                    .rect = { cx, cy, card_w, card_h },
                    .catalog_index = ui->filtered_indices[fi]
                };
            }
        }
    }

    if (ui->import_modal_open) {
        import_job_t *job = &ui->lib->import_job;
        pthread_mutex_lock(&job->lock);
        import_state_t st = job->state;
        bool active = job->active;
        pthread_mutex_unlock(&job->lock);

        bool show_form = !active;
        bool show_progress = (st != IMPORT_IDLE || active || ui->local_import_err[0] != '\0');

        double mw = fmin(580.0, (double)win_w - 40.0);
        double mh = 112.0 + (show_form ? 202.0 : 0.0) + (show_progress ? 236.0 : 0.0);
        if (mh > (double)win_h - 32.0) mh = (double)win_h - 32.0;

        double mx = ((double)win_w - mw) * 0.5;
        double my = ((double)win_h - mh) * 0.5;
        ui->r_modal_box = (ui_rect_t){ mx, my, mw, mh };
        ui->r_modal_close = (ui_rect_t){ mx + mw - 48, my + 18, 28, 28 };

        double cur_y = my + 64.0;
        double inner_x = mx + 24.0;
        double inner_w = mw - 48.0;

        if (show_form) {
            cur_y += 22.0;
#ifdef __APPLE__
            double browse_w = 96.0;
            ui->r_modal_source = (ui_rect_t){ inner_x, cur_y, inner_w - browse_w - 8.0, 36.0 };
            ui->r_modal_browse = (ui_rect_t){ inner_x + inner_w - browse_w, cur_y, browse_w, 36.0 };
#else
            double browse_w = 88.0;
            double iso_w = 72.0;
            ui->r_modal_source = (ui_rect_t){ inner_x, cur_y, inner_w - browse_w - iso_w - 16.0, 36.0 };
            ui->r_modal_browse = (ui_rect_t){ inner_x + inner_w - browse_w - iso_w - 8.0, cur_y, browse_w, 36.0 };
            ui->r_modal_browse_iso = (ui_rect_t){ inner_x + inner_w - iso_w, cur_y, iso_w, 36.0 };
#endif
            cur_y += 66.0 + 22.0;
            ui->r_modal_title = (ui_rect_t){ inner_x, cur_y, inner_w, 36.0 };
            cur_y += 48.0;
            ui->r_modal_enrich = (ui_rect_t){ inner_x, cur_y, inner_w, 26.0 };
        }

        double footer_y = my + mh - 52.0;
        double rx = mx + mw - 24.0;
        if (st == IMPORT_DONE && !active) {
            double pw = 122.0;
            ui->r_modal_play = (ui_rect_t){ rx - pw, footer_y, pw, 36.0 };
            rx -= pw + 10.0;
        }
        if (!active) {
            double sw_btn = 124.0;
            ui->r_modal_start = (ui_rect_t){ rx - sw_btn, footer_y, sw_btn, 36.0 };
            rx -= sw_btn + 10.0;
        }
        double cw = 84.0;
        ui->r_modal_done = (ui_rect_t){ rx - cw, footer_y, cw, 36.0 };
        rx -= cw + 10.0;
        if (active) {
            double xw = 124.0;
            ui->r_modal_cancel = (ui_rect_t){ rx - xw, footer_y, xw, 36.0 };
        }
    }
}

static void draw_button(cairo_t *cr, ui_rect_t r, const char *label,
                        bool primary, bool danger, bool hovered) {
    if (r.w <= 0 || r.h <= 0) return;
    cairo_save(cr);
    cairo_rounded_rect(cr, r.x, r.y, r.w, r.h, 8.0);
    if (primary) {
        cairo_set_source_rgba(cr, 0.40, 0.99, 0.95, hovered ? 1.0 : 0.90);
    } else if (danger) {
        cairo_set_source_rgba(cr, 1.0, 0.30, 0.30, hovered ? 0.32 : 0.18);
    } else {
        cairo_set_source_rgba(cr, 0.12, 0.16, 0.20, hovered ? 0.98 : 0.85);
    }
    cairo_fill_preserve(cr);

    if (!primary) {
        if (danger) {
            cairo_set_source_rgba(cr, 1.0, 0.30, 0.30, 0.55);
        } else {
            cairo_set_source_rgba(cr, hovered ? 0.40 : 1.0, hovered ? 0.99 : 1.0, hovered ? 0.95 : 1.0, hovered ? 0.9 : 0.18);
        }
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
    } else {
        cairo_new_path(cr);
    }

    double tr = primary ? 0.043 : (danger ? 1.0 : 1.0);
    double tg = primary ? 0.047 : (danger ? 0.50 : 1.0);
    double tb = primary ? 0.063 : (danger ? 0.50 : 1.0);
    draw_text_box(cr, label, r.x + 6.0, r.y + (r.h - 15.0) * 0.5, r.w - 12.0, 1, 13.5,
                  primary || danger, false, tr, tg, tb, 1.0, PANGO_ALIGN_CENTER, NULL);
    cairo_restore(cr);
}

static void render_library_view(native_ui_t *ui, cairo_t *cr, int win_w, int win_h) {
    /* Subtle radial dark background */
    cairo_pattern_t *bg = cairo_pattern_create_radial(win_w * 0.5, 0.0, 20.0, win_w * 0.5, 0.0, fmax(win_w, win_h));
    cairo_pattern_add_color_stop_rgb(bg, 0.0, 0.102, 0.118, 0.141);
    cairo_pattern_add_color_stop_rgb(bg, 1.0, 0.043, 0.047, 0.063);
    cairo_set_source(cr, bg);
    cairo_paint(cr);
    cairo_pattern_destroy(bg);

    /* Movie grid clipped below header (y >= 72) */
    cairo_save(cr);
    cairo_rectangle(cr, 0, 72, win_w, fmax(0.0, win_h - 72.0));
    cairo_clip(cr);

    if (ui->snapshot_count == 0) {
        draw_text_box(cr, "No TinyDVD movies found in library.", 24, 150, win_w - 48, 2, 18.0,
                      false, false, 0.77, 0.78, 0.78, 0.9, PANGO_ALIGN_CENTER, NULL);
        draw_text_box(cr, "Place .tdvd files in this folder or click + Import DVD to rip & add a disc.",
                      24, 182, win_w - 48, 2, 13.5, false, false, 0.53, 0.57, 0.69, 0.9, PANGO_ALIGN_CENTER, NULL);
        draw_button(cr, ui->r_empty_import, "+ Import DVD", true, false,
                    !ui->import_modal_open && pt_in_rect(ui->mouse_x, ui->mouse_y, ui->r_empty_import));
    } else if (ui->filtered_count == 0) {
        char empty_msg[640];
        snprintf(empty_msg, sizeof(empty_msg), "No movies found matching \"%s\"", ui->tf_search.text);
        draw_text_box(cr, empty_msg, 24, 160, win_w - 48, 2, 16.0, false, false,
                      0.77, 0.78, 0.78, 0.9, PANGO_ALIGN_CENTER, NULL);
    } else {
        for (int i = 0; i < ui->card_hit_count; i++) {
            ui_card_hit_t *hit = &ui->card_hits[i];
            ui_rect_t r = hit->rect;
            if (r.y + r.h < 72.0 || r.y > win_h) continue;

            const catalog_item_t *item = &ui->items_snapshot[hit->catalog_index];
            bool hovered = (!ui->import_modal_open &&
                            ((ui->hovered_card == i) || (ui->selected_filtered == i)));

            cairo_save(cr);
            cairo_rounded_rect(cr, r.x, r.y, r.w, r.h, 10.0);
            cairo_set_source_rgb(cr, 0.122, 0.157, 0.200);
            cairo_fill_preserve(cr);
            if (hovered) {
                cairo_set_source_rgba(cr, 0.40, 0.99, 0.95, 0.95);
                cairo_set_line_width(cr, 1.8);
            } else {
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.09);
                cairo_set_line_width(cr, 1.0);
            }
            cairo_stroke(cr);

            double info_h = 148.0;
            double thumb_h = r.h - info_h;

            /* Thumbnail area */
            cairo_save(cr);
            cairo_top_rounded_rect(cr, r.x + 1, r.y + 1, r.w - 2, thumb_h - 1, 9.0);
            cairo_clip(cr);
            cairo_set_source_rgb(cr, 0.078, 0.090, 0.102);
            cairo_paint(cr);

            cairo_surface_t *tsurf = ui_get_thumbnail_surface(ui, item);
            if (tsurf) {
                int iw = cairo_image_surface_get_width(tsurf);
                int ih = cairo_image_surface_get_height(tsurf);
                if (iw > 0 && ih > 0) {
                    double sx = r.w / (double)iw;
                    double sy = thumb_h / (double)ih;
                    double sc = fmax(sx, sy);
                    double dw = iw * sc;
                    double dh = ih * sc;
                    double ox = r.x + (r.w - dw) * 0.5;
                    double oy = r.y + (thumb_h - dh) * 0.5;
                    cairo_translate(cr, ox, oy);
                    cairo_scale(cr, sc, sc);
                    cairo_set_source_surface(cr, tsurf, 0, 0);
                    cairo_paint(cr);
                }
            } else {
                draw_text_box(cr, "DVD", r.x, r.y + thumb_h * 0.5 - 14.0, r.w, 1, 22.0, true, false,
                              0.40, 0.99, 0.95, 0.35, PANGO_ALIGN_CENTER, NULL);
            }
            cairo_restore(cr);

            if (item->year > 0) {
                char ystr[16];
                snprintf(ystr, sizeof(ystr), "%d", item->year);
                ui_rect_t yr_r = { r.x + r.w - 48, r.y + 8, 40, 20 };
                cairo_rounded_rect(cr, yr_r.x, yr_r.y, yr_r.w, yr_r.h, 5.0);
                cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.80);
                cairo_fill_preserve(cr);
                cairo_set_source_rgba(cr, 0.40, 0.99, 0.95, 0.45);
                cairo_set_line_width(cr, 1.0);
                cairo_stroke(cr);
                draw_text_box(cr, ystr, yr_r.x, yr_r.y + 3, yr_r.w, 1, 11.5, true, false,
                              0.40, 0.99, 0.95, 1.0, PANGO_ALIGN_CENTER, NULL);
            }

            /* Info Block */
            double ix = r.x + 12.0;
            double iw = r.w - 24.0;
            double iy = r.y + thumb_h + 10.0;

            draw_text_box(cr, item->title, ix, iy, iw, 1, 15.0, true, false,
                          1.0, 1.0, 1.0, 1.0, PANGO_ALIGN_LEFT, NULL);
            iy += 22.0;

            if (item->director[0]) {
                char dbuf[160];
                snprintf(dbuf, sizeof(dbuf), "Dir. %s", item->director);
                draw_text_box(cr, dbuf, ix, iy, iw, 1, 12.0, false, false,
                              0.27, 0.64, 0.62, 1.0, PANGO_ALIGN_LEFT, NULL);
                iy += 18.0;
            }

            if (item->cast[0]) {
                double ch = 0.0;
                draw_text_box(cr, item->cast, ix, iy, iw, 2, 11.5, false, false,
                              0.53, 0.57, 0.69, 0.95, PANGO_ALIGN_LEFT, &ch);
                iy += fmin(ch, 32.0) + 6.0;
            }

            /* Genre chips (up to width) */
            if (item->genres[0] && iy + 22.0 < r.y + r.h - 26.0) {
                char gcopy[128];
                snprintf(gcopy, sizeof(gcopy), "%s", item->genres);
                double gx = ix;
                char *saveptr = NULL;
                char *tok = strtok_r(gcopy, ",", &saveptr);
                while (tok) {
                    while (*tok == ' ') tok++;
                    double tw = measure_text_width(cr, tok, 10.5, false, false) + 12.0;
                    if (gx + tw > ix + iw) break;
                    cairo_rounded_rect(cr, gx, iy, tw, 17.0, 4.0);
                    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.08);
                    cairo_fill(cr);
                    draw_text_box(cr, tok, gx + 6.0, iy + 2.0, tw - 12.0, 1, 10.5, false, false,
                                  0.77, 0.78, 0.78, 0.95, PANGO_ALIGN_LEFT, NULL);
                    gx += tw + 5.0;
                    tok = strtok_r(NULL, ",", &saveptr);
                }
            }

            /* Footer row */
            double fy = r.y + r.h - 26.0;
            draw_text_box(cr, item->size_str, ix, fy, iw * 0.6, 1, 12.0, false, false,
                          0.77, 0.78, 0.78, 0.85, PANGO_ALIGN_LEFT, NULL);
            draw_text_box(cr, "Play ▶", ix, fy, iw, 1, 12.5, true, false,
                          0.40, 0.99, 0.95, 1.0, PANGO_ALIGN_RIGHT, NULL);

            cairo_restore(cr);
        }

        /* Scrollbar indicator when scrollable */
        if (ui->max_scroll_y > 1.0) {
            double track_h = win_h - 84.0;
            double ratio = track_h / (track_h + ui->max_scroll_y);
            double thumb_sz = fmax(32.0, track_h * ratio);
            double thumb_y = 78.0 + (track_h - thumb_sz) * (ui->scroll_y / ui->max_scroll_y);
            cairo_rounded_rect(cr, win_w - 9.0, thumb_y, 5.0, thumb_sz, 2.5);
            cairo_set_source_rgba(cr, 0.40, 0.99, 0.95, 0.35);
            cairo_fill(cr);
        }
    }
    cairo_restore(cr);

    /* Header Bar */
    cairo_set_source_rgb(cr, 0.067, 0.078, 0.102);
    cairo_rectangle(cr, 0, 0, win_w, 72);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10);
    cairo_rectangle(cr, 0, 71, win_w, 1);
    cairo_fill(cr);

    draw_text_box(cr, "Tiny", 24, 21, 0, 0, 24.0, true, false, 1.0, 1.0, 1.0, 1.0, PANGO_ALIGN_LEFT, NULL);
    double w_tiny = measure_text_width(cr, "Tiny", 24.0, true, false);
    draw_text_box(cr, "DVD", 24 + w_tiny, 21, 0, 0, 24.0, true, false, 0.40, 0.99, 0.95, 1.0, PANGO_ALIGN_LEFT, NULL);
    double w_dvd = measure_text_width(cr, "DVD", 24.0, true, false);
    draw_text_box(cr, " Cinema", 24 + w_tiny + w_dvd, 21, 0, 0, 24.0, true, false, 1.0, 1.0, 1.0, 1.0, PANGO_ALIGN_LEFT, NULL);

    /* Search Input */
    draw_textfield(cr, ui->r_search, &ui->tf_search, "Search title, actor, director, genre...",
                   ui->focus == FOCUS_SEARCH, true, 32.0, ui->tf_search.len > 0 ? 32.0 : 14.0);
    draw_text_box(cr, "⌕", ui->r_search.x + 11.0, ui->r_search.y + 8.0, 18.0, 1, 16.0, false, false,
                  0.65, 0.70, 0.80, 0.75, PANGO_ALIGN_LEFT, NULL);
    if (ui->tf_search.len > 0) {
        bool x_hov = pt_in_rect(ui->mouse_x, ui->mouse_y, ui->r_search_clear);
        draw_text_box(cr, "×", ui->r_search_clear.x, ui->r_search_clear.y + 4.0, ui->r_search_clear.w, 1, 17.0,
                      true, false, 1.0, 1.0, 1.0, x_hov ? 1.0 : 0.65, PANGO_ALIGN_CENTER, NULL);
    }

    /* Movie stats + Import button */
    char stats[64];
    const char *q = ui->tf_search.text;
    while (*q && isspace((unsigned char)*q)) q++;
    if (q[0]) {
        snprintf(stats, sizeof(stats), "%d of %d Movie%s", ui->filtered_count, ui->snapshot_count,
                 ui->snapshot_count == 1 ? "" : "s");
    } else {
        snprintf(stats, sizeof(stats), "%d Movie%s", ui->snapshot_count, ui->snapshot_count == 1 ? "" : "s");
    }
    draw_text_box(cr, stats, ui->r_open_import.x - 125.0, 27.0, 112.0, 1, 13.5, false, false,
                  0.77, 0.78, 0.78, 0.90, PANGO_ALIGN_RIGHT, NULL);

    import_job_t *job = &ui->lib->import_job;
    pthread_mutex_lock(&job->lock);
    bool import_active = job->active;
    pthread_mutex_unlock(&job->lock);

    draw_button(cr, ui->r_open_import, import_active ? "Importing..." : "+ Import DVD",
                true, false, !ui->import_modal_open && pt_in_rect(ui->mouse_x, ui->mouse_y, ui->r_open_import));
}

static void render_player_hud(native_ui_t *ui, cairo_t *cr, int win_w, const char *current_movie_title) {
    cairo_pattern_t *top = cairo_pattern_create_linear(0, 0, 0, 68);
    cairo_pattern_add_color_stop_rgba(top, 0.0, 0.0, 0.0, 0.0, 0.82);
    cairo_pattern_add_color_stop_rgba(top, 1.0, 0.0, 0.0, 0.0, 0.0);
    cairo_set_source(cr, top);
    cairo_rectangle(cr, 0, 0, win_w, 68);
    cairo_fill(cr);
    cairo_pattern_destroy(top);

    draw_button(cr, ui->r_back_lib, "← Library", false, false,
                !ui->import_modal_open && pt_in_rect(ui->mouse_x, ui->mouse_y, ui->r_back_lib));

    if (current_movie_title && current_movie_title[0]) {
        draw_text_box(cr, current_movie_title, 138, 20, win_w - 308, 1, 15.0, true, false,
                      1.0, 1.0, 1.0, 0.95, PANGO_ALIGN_LEFT, NULL);
    }

    import_job_t *job = &ui->lib->import_job;
    pthread_mutex_lock(&job->lock);
    bool import_active = job->active;
    pthread_mutex_unlock(&job->lock);
    draw_button(cr, ui->r_open_import, import_active ? "Importing..." : "+ Import DVD",
                true, false, !ui->import_modal_open && pt_in_rect(ui->mouse_x, ui->mouse_y, ui->r_open_import));
}

static void render_import_modal(native_ui_t *ui, cairo_t *cr, int win_w, int win_h) {
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.72);
    cairo_rectangle(cr, 0, 0, win_w, win_h);
    cairo_fill(cr);

    ui_rect_t b = ui->r_modal_box;
    cairo_rounded_rect(cr, b.x, b.y, b.w, b.h, 12.0);
    cairo_set_source_rgb(cr, 0.122, 0.157, 0.200);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.22);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);

    draw_text_box(cr, "Import DVD to Library", b.x + 24, b.y + 20, b.w - 88, 1, 18.0, true, false,
                  1.0, 1.0, 1.0, 1.0, PANGO_ALIGN_LEFT, NULL);
    draw_button(cr, ui->r_modal_close, "×", false, false, pt_in_rect(ui->mouse_x, ui->mouse_y, ui->r_modal_close));

    import_job_t *job = &ui->lib->import_job;
    pthread_mutex_lock(&job->lock);
    import_state_t st = job->state;
    bool active = job->active;
    int elapsed = (job->start_time > 0) ? (int)(time(NULL) - job->start_time) : 0;
    char stage_msg[256];
    char err_msg[512];
    snprintf(stage_msg, sizeof(stage_msg), "%s", job->stage_msg);
    snprintf(err_msg, sizeof(err_msg), "%s", job->error_msg);
    int log_count = job->log_count;
    char log_concat[IMPORT_MAX_LOGS * IMPORT_LOG_LINE_LEN];
    size_t lpos = 0;
    log_concat[0] = '\0';
    for (int i = 0; i < log_count; i++) {
        size_t ll = strlen(job->log_lines[i]);
        if (lpos + ll + 2 < sizeof(log_concat)) {
            if (i > 0) log_concat[lpos++] = '\n';
            memcpy(log_concat + lpos, job->log_lines[i], ll);
            lpos += ll;
            log_concat[lpos] = '\0';
        }
    }
    pthread_mutex_unlock(&job->lock);

    double ix = b.x + 24.0;
    double iw = b.w - 48.0;
    double cur_y = b.y + 64.0;

    if (!active) {
        draw_text_box(cr, "DVD Drive or Source Path", ix, cur_y, iw, 1, 13.0, true, false,
                      1.0, 1.0, 1.0, 0.95, PANGO_ALIGN_LEFT, NULL);
        draw_textfield(cr, ui->r_modal_source, &ui->tf_source, "/dev/sr0 or /path/to/DVD_FOLDER or movie.iso",
                       ui->focus == FOCUS_MODAL_SOURCE, false, 12.0, 10.0);
#ifdef __APPLE__
        draw_button(cr, ui->r_modal_browse, "Browse...", false, false,
                    pt_in_rect(ui->mouse_x, ui->mouse_y, ui->r_modal_browse));
#else
        draw_button(cr, ui->r_modal_browse, "Folder...", false, false,
                    pt_in_rect(ui->mouse_x, ui->mouse_y, ui->r_modal_browse));
        draw_button(cr, ui->r_modal_browse_iso, "ISO...", false, false,
                    pt_in_rect(ui->mouse_x, ui->mouse_y, ui->r_modal_browse_iso));
#endif
        draw_text_box(cr, "Optical drive device, mounted disc volume, DVD backup folder, or .iso file.",
                      ix, ui->r_modal_source.y + 40.0, iw, 1, 11.5, false, false,
                      0.53, 0.57, 0.69, 0.95, PANGO_ALIGN_LEFT, NULL);

        draw_text_box(cr, "Movie Title (optional)", ix, ui->r_modal_title.y - 22.0, iw, 1, 13.0, true, false,
                      1.0, 1.0, 1.0, 0.95, PANGO_ALIGN_LEFT, NULL);
        draw_textfield(cr, ui->r_modal_title, &ui->tf_title, "Auto-detect from disc label (e.g. Eulogy)",
                       ui->focus == FOCUS_MODAL_TITLE, false, 12.0, 10.0);

        /* Checkbox */
        ui_rect_t cb = { ui->r_modal_enrich.x, ui->r_modal_enrich.y + 4.0, 18.0, 18.0 };
        cairo_rounded_rect(cr, cb.x, cb.y, cb.w, cb.h, 4.0);
        if (ui->enrich_checked) {
            cairo_set_source_rgb(cr, 0.40, 0.99, 0.95);
            cairo_fill(cr);
            draw_text_box(cr, "✓", cb.x, cb.y + 1.0, cb.w, 1, 13.0, true, false,
                          0.043, 0.047, 0.063, 1.0, PANGO_ALIGN_CENTER, NULL);
        } else {
            cairo_set_source_rgb(cr, 0.043, 0.047, 0.063);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.35);
            cairo_set_line_width(cr, 1.2);
            cairo_stroke(cr);
        }
        draw_text_box(cr, "Fetch movie metadata (director, cast, year, genres)",
                      cb.x + 26.0, ui->r_modal_enrich.y + 4.0, iw - 28.0, 1, 13.0, false, false,
                      0.77, 0.78, 0.78, 0.98, PANGO_ALIGN_LEFT, NULL);
        cur_y = ui->r_modal_enrich.y + 34.0;
    }

    const char *display_err = ui->local_import_err[0] ? ui->local_import_err : err_msg;
    if (st != IMPORT_IDLE || active || display_err[0]) {
        double pbox_h = b.y + b.h - 66.0 - cur_y;
        if (pbox_h > 40.0) {
            cairo_rounded_rect(cr, ix, cur_y, iw, pbox_h, 8.0);
            cairo_set_source_rgb(cr, 0.043, 0.047, 0.063);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);

            bool is_err = (st == IMPORT_ERROR || ui->local_import_err[0] != '\0');
            const char *s_text = stage_msg[0] ? stage_msg : (ui->local_import_err[0] ? "Error" : "Importing...");
            draw_text_box(cr, s_text, ix + 12, cur_y + 10, iw - 90, 1, 13.5, true, false,
                          is_err ? 1.0 : 0.40, is_err ? 0.30 : 0.99, is_err ? 0.30 : 0.95, 1.0,
                          PANGO_ALIGN_LEFT, NULL);

            char tbuf[32];
            snprintf(tbuf, sizeof(tbuf), "%02d:%02d", elapsed / 60, elapsed % 60);
            draw_text_box(cr, tbuf, ix + iw - 74, cur_y + 10, 62, 1, 13.0, false, true,
                          0.77, 0.78, 0.78, 0.9, PANGO_ALIGN_RIGHT, NULL);

            double log_y = cur_y + 36.0;
            if (is_err && display_err[0]) {
                cairo_rounded_rect(cr, ix + 12, log_y, iw - 24, 30, 5.0);
                cairo_set_source_rgba(cr, 1.0, 0.30, 0.30, 0.14);
                cairo_fill(cr);
                draw_text_box(cr, display_err, ix + 20, log_y + 7, iw - 40, 1, 12.0, false, false,
                              1.0, 0.45, 0.45, 1.0, PANGO_ALIGN_LEFT, NULL);
                log_y += 36.0;
            }

            double log_h = cur_y + pbox_h - 10.0 - log_y;
            if (log_h > 24.0) {
                cairo_save(cr);
                cairo_rounded_rect(cr, ix + 10, log_y, iw - 20, log_h, 6.0);
                cairo_set_source_rgb(cr, 0.024, 0.027, 0.039);
                cairo_fill_preserve(cr);
                cairo_clip(cr);

                /* Measure log text height and align bottom so latest lines are visible */
                PangoLayout *layout = pango_cairo_create_layout(cr);
                PangoFontDescription *desc = pango_font_description_new();
                pango_font_description_set_family(desc, "Monospace");
                pango_font_description_set_absolute_size(desc, 11.5 * PANGO_SCALE);
                pango_layout_set_font_description(layout, desc);
                pango_font_description_free(desc);
                pango_layout_set_text(layout, log_concat, -1);
                pango_layout_set_width(layout, (int)((iw - 36.0) * PANGO_SCALE));
                pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);

                int lw = 0, lh = 0;
                pango_layout_get_pixel_size(layout, &lw, &lh);
                double draw_y = log_y + 6.0;
                if ((double)lh > log_h - 12.0) {
                    draw_y = (log_y + log_h - 6.0) - (double)lh + ui->log_scroll_offset;
                    if (draw_y > log_y + 6.0) draw_y = log_y + 6.0;
                }
                cairo_set_source_rgba(cr, 0.66, 0.70, 0.82, 0.95);
                cairo_move_to(cr, ix + 18.0, draw_y);
                pango_cairo_show_layout(cr, layout);
                g_object_unref(layout);
                cairo_restore(cr);
            }
        }
    }

    if (active) {
        draw_button(cr, ui->r_modal_cancel, "Cancel Import", false, true,
                    pt_in_rect(ui->mouse_x, ui->mouse_y, ui->r_modal_cancel));
    }
    draw_button(cr, ui->r_modal_done, "Close", false, false,
                pt_in_rect(ui->mouse_x, ui->mouse_y, ui->r_modal_done));
    if (!active) {
        draw_button(cr, ui->r_modal_start, "Start Import", true, false,
                    pt_in_rect(ui->mouse_x, ui->mouse_y, ui->r_modal_start));
    }
    if (st == IMPORT_DONE && !active) {
        draw_button(cr, ui->r_modal_play, "Play Now ▶", true, false,
                    pt_in_rect(ui->mouse_x, ui->mouse_y, ui->r_modal_play));
    }
}

void native_ui_render(native_ui_t *ui, int draw_w, int draw_h,
                      bool in_player_view, const char *current_movie_title,
                      bool hud_visible) {
    if (!ui || !ui->window || draw_w <= 0 || draw_h <= 0) return;

    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(ui->window, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0) return;

    import_job_t *job = &ui->lib->import_job;
    pthread_mutex_lock(&job->lock);
    import_state_t imp_state = job->state;
    bool imp_active = job->active;
    pthread_mutex_unlock(&job->lock);

    if (ui->last_import_state != IMPORT_DONE && imp_state == IMPORT_DONE) {
        sync_catalog_snapshot(ui);
        ui->dirty = true;
    }
    ui->last_import_state = imp_state;

    uint32_t now = SDL_GetTicks();
    if (imp_active && (now - ui->last_tick_ms >= 250)) {
        ui->last_tick_ms = now;
        ui->dirty = true;
    }

    if (win_w != ui->last_win_w || win_h != ui->last_win_h ||
        in_player_view != ui->last_player_view || hud_visible != ui->last_hud_visible) {
        ui->last_win_w = win_w;
        ui->last_win_h = win_h;
        ui->last_player_view = in_player_view;
        ui->last_hud_visible = hud_visible;
        ui->dirty = true;
    }

    if (in_player_view && !hud_visible && !ui->import_modal_open) {
        return;
    }

    if (ui->dirty || draw_w != ui->tex_w || draw_h != ui->tex_h) {
        if (!in_player_view) {
            sync_catalog_snapshot(ui);
        }
        compute_layout_rects(ui, win_w, win_h, in_player_view);

        if (draw_w != ui->tex_w || draw_h != ui->tex_h) {
            free(ui->pixel_buf);
            ui->pixel_buf = (uint8_t *)calloc((size_t)draw_w * (size_t)draw_h * 4, 1);
            ui->tex_w = draw_w;
            ui->tex_h = draw_h;
        } else if (ui->pixel_buf) {
            memset(ui->pixel_buf, 0, (size_t)draw_w * (size_t)draw_h * 4);
        }

        if (ui->pixel_buf) {
            cairo_surface_t *surf = cairo_image_surface_create_for_data(
                ui->pixel_buf, CAIRO_FORMAT_ARGB32, draw_w, draw_h, draw_w * 4);
            cairo_t *cr = cairo_create(surf);
            double sx = (double)draw_w / (double)win_w;
            double sy = (double)draw_h / (double)win_h;
            cairo_scale(cr, sx, sy);

            if (!in_player_view) {
                render_library_view(ui, cr, win_w, win_h);
            } else if (hud_visible) {
                render_player_hud(ui, cr, win_w, current_movie_title);
            }
            if (ui->import_modal_open) {
                render_import_modal(ui, cr, win_w, win_h);
            }

            cairo_surface_flush(surf);
            cairo_destroy(cr);
            cairo_surface_destroy(surf);
        }

        if (!ui->gl_initialized) {
            glGenTextures(1, &ui->gl_tex);
            glBindTexture(GL_TEXTURE_2D, ui->gl_tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            ui->gl_initialized = true;
        } else {
            glBindTexture(GL_TEXTURE_2D, ui->gl_tex);
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, draw_w, draw_h, 0, GL_BGRA,
                     GL_UNSIGNED_INT_8_8_8_8_REV, ui->pixel_buf);
        ui->dirty = false;
    }

    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glViewport(0, 0, draw_w, draw_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, ui->gl_tex);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, draw_w, draw_h, 0, -1, 1);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(0.0f,          0.0f);
    glTexCoord2f(1.0f, 0.0f); glVertex2f((float)draw_w, 0.0f);
    glTexCoord2f(1.0f, 1.0f); glVertex2f((float)draw_w, (float)draw_h);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(0.0f,          (float)draw_h);
    glEnd();

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);

    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
}

/* =========================================================================
 * Event Handling
 * ========================================================================= */

static ui_textfield_t *active_textfield(native_ui_t *ui) {
    if (!ui) return NULL;
    switch (ui->focus) {
        case FOCUS_SEARCH:       return &ui->tf_search;
        case FOCUS_MODAL_SOURCE: return &ui->tf_source;
        case FOCUS_MODAL_TITLE:  return &ui->tf_title;
        default:                 return NULL;
    }
}

bool native_ui_handle_event(native_ui_t *ui, const SDL_Event *ev,
                            bool in_player_view,
                            char *out_play_disc_path, size_t path_len,
                            char *out_play_disc_title, size_t title_len,
                            bool *out_back_to_library) {
    if (out_play_disc_path && path_len > 0) out_play_disc_path[0] = '\0';
    if (out_play_disc_title && title_len > 0) out_play_disc_title[0] = '\0';
    if (out_back_to_library) *out_back_to_library = false;
    if (!ui || !ev) return false;

    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(ui->window, &win_w, &win_h);

    /* Ensure layout rects exist even before first render */
    if (win_w > 0 && win_h > 0 && ui->last_win_w == 0) {
        compute_layout_rects(ui, win_w, win_h, in_player_view);
    }

    if (ev->type == SDL_MOUSEMOTION) {
        ui->mouse_x = ev->motion.x;
        ui->mouse_y = ev->motion.y;
        if (!in_player_view && !ui->import_modal_open) {
            int new_hov = -1;
            for (int i = 0; i < ui->card_hit_count; i++) {
                if (ui->mouse_y >= 72 && pt_in_rect(ui->mouse_x, ui->mouse_y, ui->card_hits[i].rect)) {
                    new_hov = i;
                    break;
                }
            }
            if (new_hov != ui->hovered_card) {
                ui->hovered_card = new_hov;
                ui->dirty = true;
            }
            ui->dirty = true;
            return true;
        }
        if (ui->import_modal_open || (in_player_view && ui->mouse_y < 60)) {
            ui->dirty = true;
        }
        return ui->import_modal_open;
    }

    if (ev->type == SDL_MOUSEWHEEL) {
        if (ui->import_modal_open) {
            ui->log_scroll_offset += ev->wheel.y * 20.0;
            if (ui->log_scroll_offset < 0.0) ui->log_scroll_offset = 0.0;
            ui->dirty = true;
            return true;
        }
        if (!in_player_view) {
            ui->scroll_y -= ev->wheel.y * 42.0;
            if (ui->scroll_y < 0.0) ui->scroll_y = 0.0;
            if (ui->scroll_y > ui->max_scroll_y) ui->scroll_y = ui->max_scroll_y;
            ui->dirty = true;
            return true;
        }
        return false;
    }

    if (ev->type == SDL_MOUSEBUTTONDOWN && ev->button.button == SDL_BUTTON_LEFT) {
        double mx = ev->button.x;
        double my = ev->button.y;

        if (ui->import_modal_open) {
            import_job_t *job = &ui->lib->import_job;
            pthread_mutex_lock(&job->lock);
            import_state_t imp_st = job->state;
            bool imp_active = job->active;
            char res_path[1536];
            char res_title[256];
            snprintf(res_path, sizeof(res_path), "%s", job->result_disc_path);
            snprintf(res_title, sizeof(res_title), "%s", job->result_title);
            pthread_mutex_unlock(&job->lock);

            if (!pt_in_rect(mx, my, ui->r_modal_box) || pt_in_rect(mx, my, ui->r_modal_close) ||
                pt_in_rect(mx, my, ui->r_modal_done)) {
                close_import_modal(ui);
                return true;
            }
            if (imp_active && pt_in_rect(mx, my, ui->r_modal_cancel)) {
                tdvd_library_import_cancel(ui->lib);
                ui->dirty = true;
                return true;
            }
            if (!imp_active && imp_st == IMPORT_DONE && pt_in_rect(mx, my, ui->r_modal_play) && res_path[0]) {
                close_import_modal(ui);
                if (out_play_disc_path && path_len > 0) snprintf(out_play_disc_path, path_len, "%s", res_path);
                if (out_play_disc_title && title_len > 0) snprintf(out_play_disc_title, title_len, "%s", res_title);
                return true;
            }
            if (!imp_active) {
                if (pt_in_rect(mx, my, ui->r_modal_source)) {
                    ui->focus = FOCUS_MODAL_SOURCE;
                    ui->dirty = true;
                    return true;
                }
                if (pt_in_rect(mx, my, ui->r_modal_title)) {
                    ui->focus = FOCUS_MODAL_TITLE;
                    ui->dirty = true;
                    return true;
                }
                if (pt_in_rect(mx, my, ui->r_modal_browse)) {
                    char chosen[512] = {0};
                    if (native_dialog_choose_source(false, ui->tf_source.text, chosen, sizeof(chosen))) {
                        tf_set(&ui->tf_source, chosen);
                    }
                    ui->dirty = true;
                    return true;
                }
                if (pt_in_rect(mx, my, ui->r_modal_browse_iso)) {
                    char chosen[512] = {0};
                    if (native_dialog_choose_source(true, ui->tf_source.text, chosen, sizeof(chosen))) {
                        tf_set(&ui->tf_source, chosen);
                    }
                    ui->dirty = true;
                    return true;
                }
                if (pt_in_rect(mx, my, ui->r_modal_enrich)) {
                    ui->enrich_checked = !ui->enrich_checked;
                    ui->dirty = true;
                    return true;
                }
                if (pt_in_rect(mx, my, ui->r_modal_start)) {
                    start_import_from_modal(ui);
                    return true;
                }
            }
            ui->focus = FOCUS_NONE;
            ui->dirty = true;
            return true;
        }

        if (in_player_view) {
            if (ui->last_hud_visible) {
                if (pt_in_rect(mx, my, ui->r_back_lib)) {
                    if (out_back_to_library) *out_back_to_library = true;
                    return true;
                }
                if (pt_in_rect(mx, my, ui->r_open_import)) {
                    open_import_modal(ui);
                    return true;
                }
            }
            return false;
        }

        /* Library view clicks */
        if (pt_in_rect(mx, my, ui->r_open_import) ||
            (ui->snapshot_count == 0 && pt_in_rect(mx, my, ui->r_empty_import))) {
            open_import_modal(ui);
            return true;
        }
        if (ui->tf_search.len > 0 && pt_in_rect(mx, my, ui->r_search_clear)) {
            tf_set(&ui->tf_search, "");
            ui->scroll_y = 0.0;
            sync_catalog_snapshot(ui);
            ui->dirty = true;
            return true;
        }
        if (pt_in_rect(mx, my, ui->r_search)) {
            ui->focus = FOCUS_SEARCH;
            ui->dirty = true;
            return true;
        }

        ui->focus = FOCUS_NONE;
        if (my >= 72.0) {
            for (int i = 0; i < ui->card_hit_count; i++) {
                if (pt_in_rect(mx, my, ui->card_hits[i].rect)) {
                    const catalog_item_t *it = &ui->items_snapshot[ui->card_hits[i].catalog_index];
                    if (out_play_disc_path && path_len > 0) snprintf(out_play_disc_path, path_len, "%s", it->path);
                    if (out_play_disc_title && title_len > 0) snprintf(out_play_disc_title, title_len, "%s", it->title);
                    return true;
                }
            }
        }
        ui->dirty = true;
        return true;
    }

    if (ev->type == SDL_TEXTINPUT) {
        ui_textfield_t *tf = active_textfield(ui);
        if (tf) {
            tf_insert_str(tf, ev->text.text);
            if (ui->focus == FOCUS_SEARCH) {
                ui->scroll_y = 0.0;
                sync_catalog_snapshot(ui);
            }
            ui->dirty = true;
            return true;
        }
        if (!in_player_view && !ui->import_modal_open && strcmp(ev->text.text, "/") == 0) {
            ui->focus = FOCUS_SEARCH;
            ui->dirty = true;
            return true;
        }
        return ui->import_modal_open;
    }

    if (ev->type == SDL_KEYDOWN) {
        SDL_Keycode sym = ev->key.keysym.sym;
        Uint16 mod = ev->key.keysym.mod;
        bool accel = (mod & KMOD_GUI) || (mod & KMOD_CTRL);

        ui_textfield_t *tf = active_textfield(ui);
        if (tf) {
            if (accel && sym == SDLK_a) {
                tf->select_all = true;
                ui->dirty = true;
                return true;
            }
            if (accel && sym == SDLK_v) {
                char *clip = SDL_GetClipboardText();
                if (clip) {
                    tf_insert_str(tf, clip);
                    SDL_free(clip);
                    if (ui->focus == FOCUS_SEARCH) {
                        ui->scroll_y = 0.0;
                        sync_catalog_snapshot(ui);
                    }
                    ui->dirty = true;
                }
                return true;
            }
            if (accel && sym == SDLK_c) {
                SDL_SetClipboardText(tf->text);
                return true;
            }
            if ((mod & KMOD_CTRL) && sym == SDLK_u) {
                tf_set(tf, "");
                if (ui->focus == FOCUS_SEARCH) sync_catalog_snapshot(ui);
                ui->dirty = true;
                return true;
            }
            if (sym == SDLK_BACKSPACE) {
                tf_backspace(tf);
                if (ui->focus == FOCUS_SEARCH) {
                    ui->scroll_y = 0.0;
                    sync_catalog_snapshot(ui);
                }
                ui->dirty = true;
                return true;
            }
            if (sym == SDLK_DELETE) {
                tf_delete_fwd(tf);
                if (ui->focus == FOCUS_SEARCH) sync_catalog_snapshot(ui);
                ui->dirty = true;
                return true;
            }
            if (sym == SDLK_LEFT) { tf_move_left(tf); ui->dirty = true; return true; }
            if (sym == SDLK_RIGHT) { tf_move_right(tf); ui->dirty = true; return true; }
            if (sym == SDLK_HOME) { tf->select_all = false; tf->cursor = 0; ui->dirty = true; return true; }
            if (sym == SDLK_END) { tf->select_all = false; tf->cursor = tf->len; ui->dirty = true; return true; }
            if (sym == SDLK_TAB && ui->import_modal_open) {
                ui->focus = (ui->focus == FOCUS_MODAL_SOURCE) ? FOCUS_MODAL_TITLE : FOCUS_MODAL_SOURCE;
                ui->dirty = true;
                return true;
            }
            if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                if (ui->import_modal_open) {
                    start_import_from_modal(ui);
                    return true;
                }
                if (ui->focus == FOCUS_SEARCH && ui->filtered_count > 0) {
                    int idx = (ui->selected_filtered >= 0 && ui->selected_filtered < ui->filtered_count)
                              ? ui->selected_filtered : 0;
                    const catalog_item_t *it = &ui->items_snapshot[ui->filtered_indices[idx]];
                    if (out_play_disc_path && path_len > 0) snprintf(out_play_disc_path, path_len, "%s", it->path);
                    if (out_play_disc_title && title_len > 0) snprintf(out_play_disc_title, title_len, "%s", it->title);
                    return true;
                }
                return true;
            }
            if (sym == SDLK_ESCAPE) {
                if (ui->import_modal_open) {
                    close_import_modal(ui);
                } else if (ui->focus == FOCUS_SEARCH && ui->tf_search.len > 0) {
                    tf_set(&ui->tf_search, "");
                    ui->scroll_y = 0.0;
                    sync_catalog_snapshot(ui);
                    ui->dirty = true;
                } else {
                    ui->focus = FOCUS_NONE;
                    ui->dirty = true;
                }
                return true;
            }
            return true; /* Consume all keys while typing in a text field */
        }

        if (ui->import_modal_open) {
            if (sym == SDLK_ESCAPE) {
                close_import_modal(ui);
            } else if (sym == SDLK_TAB) {
                ui->focus = FOCUS_MODAL_SOURCE;
                ui->dirty = true;
            }
            return true;
        }

        if (!in_player_view) {
            if (accel && sym == SDLK_f) {
                ui->focus = FOCUS_SEARCH;
                ui->dirty = true;
                return true;
            }
            if (sym == SDLK_DOWN || sym == SDLK_PAGEDOWN) {
                ui->scroll_y += (sym == SDLK_PAGEDOWN) ? 240.0 : 60.0;
                if (ui->scroll_y > ui->max_scroll_y) ui->scroll_y = ui->max_scroll_y;
                ui->dirty = true;
                return true;
            }
            if (sym == SDLK_UP || sym == SDLK_PAGEUP) {
                ui->scroll_y -= (sym == SDLK_PAGEUP) ? 240.0 : 60.0;
                if (ui->scroll_y < 0.0) ui->scroll_y = 0.0;
                ui->dirty = true;
                return true;
            }
            if ((sym == SDLK_RETURN || sym == SDLK_KP_ENTER) && ui->filtered_count > 0) {
                int idx = (ui->selected_filtered >= 0 && ui->selected_filtered < ui->filtered_count)
                          ? ui->selected_filtered : 0;
                const catalog_item_t *it = &ui->items_snapshot[ui->filtered_indices[idx]];
                if (out_play_disc_path && path_len > 0) snprintf(out_play_disc_path, path_len, "%s", it->path);
                if (out_play_disc_title && title_len > 0) snprintf(out_play_disc_title, title_len, "%s", it->title);
                return true;
            }
            if (sym == SDLK_q || sym == SDLK_ESCAPE || sym == SDLK_f) {
                return false; /* Allow window quit / fullscreen toggle */
            }
            return true;
        }
    }

    return false;
}
