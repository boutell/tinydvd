#ifndef TINYDVD_LIBRARY_H
#define TINYDVD_LIBRARY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IMPORT_MAX_LOGS 64
#define IMPORT_LOG_LINE_LEN 256

typedef enum {
    IMPORT_IDLE = 0,
    IMPORT_RIPPING,
    IMPORT_CONVERTING,
    IMPORT_DONE,
    IMPORT_ERROR
} import_state_t;

typedef struct import_job {
    pthread_mutex_t lock;
    import_state_t state;
    bool active;
    bool cancel_requested;
    pid_t child_pid;
    pthread_t worker_thread;
    bool thread_valid;
    time_t start_time;
    char source[512];
    char title[256];
    bool skip_enrichment;
    char stage_msg[256];
    char error_msg[512];
    char result_disc_id[128];
    char result_disc_path[1536];
    char result_title[256];
    char log_lines[IMPORT_MAX_LOGS][IMPORT_LOG_LINE_LEN];
    int log_count;
} import_job_t;

typedef struct catalog_item {
    char id[128];
    char title[256];
    char path[1024];
    uint64_t size_bytes;
    char size_str[32];
    bool has_thumbnail;
    int year;
    char director[128];
    char cast[1024];
    char genres[128];
    char overview[1024];
} catalog_item_t;

typedef struct tdvd_library {
    char target_path[1024];
    bool is_library;

    catalog_item_t *catalog;
    int catalog_count;
    int catalog_cap;
    pthread_mutex_t catalog_lock;

    char default_disc_path[1024];
    char default_disc_id[128];
    char default_disc_title[256];

    import_job_t import_job;
} tdvd_library_t;

void tdvd_library_init(tdvd_library_t *lib, const char *target_path);
void tdvd_library_destroy(tdvd_library_t *lib);
void tdvd_library_rescan(tdvd_library_t *lib);
void tdvd_library_set_directory(tdvd_library_t *lib, const char *dir_path);

bool tdvd_library_is_tdvd_file(const char *path);
bool tdvd_library_is_dvd_dir(const char *dir_path);
void tdvd_library_sanitize_id(const char *in, char *out, size_t out_len);

/**
 * Read embedded thumbnail.png bytes from a .tdvd archive or unpacked directory.
 * Caller owns the returned buffer and must free() it.
 */
uint8_t *tdvd_library_read_thumbnail(const char *disc_path, size_t *out_size);

/**
 * Auto-detect a likely DVD drive / mounted disc volume and clean title hint.
 */
void tdvd_library_detect_default_source(char *out_source, size_t source_len,
                                        char *out_label, size_t label_len);

/**
 * Start an asynchronous DVD import job into this library.
 * Returns false (with err_buf populated) if an import is already active or fails to start.
 */
bool tdvd_library_import_start(tdvd_library_t *lib, const char *source, const char *title,
                               bool skip_enrichment, char *err_buf, size_t err_len);

/**
 * Request cancellation of any active import job and SIGTERM its child process group.
 */
void tdvd_library_import_cancel(tdvd_library_t *lib);

const char *tdvd_import_state_name(import_state_t st);

#ifdef __cplusplus
}
#endif

#endif /* TINYDVD_LIBRARY_H */
