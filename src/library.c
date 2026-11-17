#include "library.h"
#include "tdvd_archive.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

void tdvd_library_sanitize_id(const char *in, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    if (!in) {
        out[0] = '\0';
        return;
    }
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < out_len; i++) {
        char c = in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') {
            out[j++] = c;
        } else {
            out[j++] = '_';
        }
    }
    out[j] = '\0';
}

bool tdvd_library_is_tdvd_file(const char *path) {
    if (!path) return false;
    size_t len = strlen(path);
    if (len > 5 && strcasecmp(path + len - 5, ".tdvd") == 0) return true;
    if (len > 8 && strcasecmp(path + len - 8, ".tinydvd") == 0) return true;
    return false;
}

bool tdvd_library_is_dvd_dir(const char *dir_path) {
    if (!dir_path) return false;
    char ifo[1536];
    struct stat st;
    snprintf(ifo, sizeof(ifo), "%s/VIDEO_TS.IFO", dir_path);
    if (stat(ifo, &st) == 0) return true;
    snprintf(ifo, sizeof(ifo), "%s/video_ts.ifo", dir_path);
    if (stat(ifo, &st) == 0) return true;
    return false;
}

static void format_size(uint64_t bytes, char *buf, size_t len) {
    if (bytes >= 1073741824ULL) {
        snprintf(buf, len, "%.1f GB", (double)bytes / 1073741824.0);
    } else if (bytes >= 1048576ULL) {
        snprintf(buf, len, "%.1f MB", (double)bytes / 1048576.0);
    } else {
        snprintf(buf, len, "%llu KB", (unsigned long long)(bytes / 1024ULL));
    }
}

static void json_get_string(const char *json, const char *key, char *out, size_t out_len) {
    if (!json || !key || !out || out_len == 0) return;
    out[0] = '\0';
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return;
    p += strlen(needle);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')) p++;
    if (*p == '"') {
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < out_len) {
            if (*p == '\\' && *(p + 1)) p++;
            out[i++] = *p++;
        }
        out[i] = '\0';
    } else if (*p == '[') {
        p++;
        size_t i = 0;
        bool in_str = false;
        while (*p && *p != ']' && i + 4 < out_len) {
            if (*p == '"') {
                if (in_str) {
                    in_str = false;
                    const char *next = p + 1;
                    while (*next && (*next == ' ' || *next == '\t' || *next == '\r' || *next == '\n')) next++;
                    if (*next == ',') {
                        out[i++] = ',';
                        out[i++] = ' ';
                    }
                } else {
                    in_str = true;
                }
            } else if (in_str) {
                if (*p == '\\' && *(p + 1)) p++;
                out[i++] = *p;
            }
            p++;
        }
        out[i] = '\0';
    }
}

static int json_get_int(const char *json, const char *key) {
    if (!json || !key) return 0;
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')) p++;
    return atoi(p);
}

static void parse_catalog_metadata(catalog_item_t *item, const char *json) {
    if (!item || !json) return;
    char temp[256] = {0};
    json_get_string(json, "title", temp, sizeof(temp));
    if (temp[0]) strncpy(item->title, temp, sizeof(item->title) - 1);

    int yr = json_get_int(json, "year");
    if (yr > 0) item->year = yr;

    json_get_string(json, "director", item->director, sizeof(item->director));
    json_get_string(json, "cast", item->cast, sizeof(item->cast));
    json_get_string(json, "genres", item->genres, sizeof(item->genres));
    json_get_string(json, "overview", item->overview, sizeof(item->overview));
}

static void add_catalog_item(tdvd_library_t *lib, const char *path, const char *title) {
    if (lib->catalog_count >= lib->catalog_cap) {
        lib->catalog_cap = lib->catalog_cap ? (lib->catalog_cap * 2) : 16;
        lib->catalog = (catalog_item_t *)realloc(lib->catalog, (size_t)lib->catalog_cap * sizeof(catalog_item_t));
    }
    catalog_item_t *item = &lib->catalog[lib->catalog_count++];
    memset(item, 0, sizeof(*item));

    strncpy(item->path, path, sizeof(item->path) - 1);
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    tdvd_library_sanitize_id(base, item->id, sizeof(item->id));

    if (title && title[0]) {
        strncpy(item->title, title, sizeof(item->title) - 1);
    } else {
        strncpy(item->title, base, sizeof(item->title) - 1);
        size_t blen = strlen(item->title);
        if (blen > 5 && strcasecmp(item->title + blen - 5, ".tdvd") == 0) {
            item->title[blen - 5] = '\0';
        } else if (blen > 8 && strcasecmp(item->title + blen - 8, ".tinydvd") == 0) {
            item->title[blen - 8] = '\0';
        }
    }

    struct stat st;
    if (stat(path, &st) == 0) {
        item->size_bytes = (uint64_t)st.st_size;
        format_size(item->size_bytes, item->size_str, sizeof(item->size_str));
    }

    if (tdvd_library_is_tdvd_file(path)) {
        tdvd_archive_t *arc = tdvd_archive_open(path);
        if (arc) {
            uint64_t toff = 0, tsz = 0;
            item->has_thumbnail = tdvd_archive_find(arc, "thumbnail.png", &toff, &tsz);

            uint64_t moff = 0, msz = 0;
            if (tdvd_archive_find(arc, "metadata.json", &moff, &msz) && msz > 0 && msz < 65536) {
                FILE *mf = fopen(path, "rb");
                if (mf) {
                    fseeko(mf, (off_t)moff, SEEK_SET);
                    char *mjson = (char *)malloc(msz + 1);
                    if (mjson) {
                        size_t nr = fread(mjson, 1, msz, mf);
                        mjson[nr] = '\0';
                        parse_catalog_metadata(item, mjson);
                        free(mjson);
                    }
                    fclose(mf);
                }
            }
            tdvd_archive_close(arc);
        }
    } else if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        char tpath[1536];
        snprintf(tpath, sizeof(tpath), "%s/thumbnail.png", path);
        struct stat tst;
        if (stat(tpath, &tst) == 0 && tst.st_size > 0) {
            item->has_thumbnail = true;
        }
    }

    /* Fallback: check for sidecar <path_without_ext>.json or metadata.json */
    if (item->director[0] == '\0') {
        char sidecar[1536];
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(sidecar, sizeof(sidecar), "%s/metadata.json", path);
        } else {
            snprintf(sidecar, sizeof(sidecar), "%s", path);
            char *dot = strrchr(sidecar, '.');
            if (dot) strcpy(dot, ".json");
            else strncat(sidecar, ".json", sizeof(sidecar) - strlen(sidecar) - 1);
        }

        FILE *sf = fopen(sidecar, "rb");
        if (sf) {
            fseek(sf, 0, SEEK_END);
            long ssz = ftell(sf);
            if (ssz > 0 && ssz < 65536) {
                fseek(sf, 0, SEEK_SET);
                char *sbuf = (char *)malloc((size_t)ssz + 1);
                if (sbuf) {
                    size_t sn = fread(sbuf, 1, (size_t)ssz, sf);
                    sbuf[sn] = '\0';
                    parse_catalog_metadata(item, sbuf);
                    free(sbuf);
                }
            }
            fclose(sf);
        }
    }
}

static int compare_catalog_items(const void *a, const void *b) {
    const catalog_item_t *ca = (const catalog_item_t *)a;
    const catalog_item_t *cb = (const catalog_item_t *)b;
    int cmp = strcasecmp(ca->title, cb->title);
    if (cmp != 0) return cmp;
    if (ca->year != cb->year) return ca->year - cb->year;
    return strcmp(ca->id, cb->id);
}

static void expand_tilde_path(const char *in, char *out, size_t out_len) {
    if (!in || !out || out_len == 0) return;
    if (in[0] == '~' && (in[1] == '/' || in[1] == '\0')) {
        const char *home = getenv("HOME");
        if (home) {
            snprintf(out, out_len, "%s%s", home, in + 1);
        } else {
            snprintf(out, out_len, "%s", in);
        }
    } else {
        snprintf(out, out_len, "%s", in);
    }
    size_t len = strlen(out);
    while (len > 1 && out[len - 1] == '/') {
        out[--len] = '\0';
    }
}

void tdvd_library_rescan(tdvd_library_t *lib) {
    if (!lib) return;
    pthread_mutex_lock(&lib->catalog_lock);
    lib->catalog_count = 0;

    struct stat st;
    if (stat(lib->target_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (!lib->is_library && tdvd_library_is_dvd_dir(lib->target_path)) {
            add_catalog_item(lib, lib->target_path, NULL);
        } else {
            lib->is_library = true;
            DIR *dir = opendir(lib->target_path);
            if (dir) {
                struct dirent *de;
                while ((de = readdir(dir)) != NULL) {
                    if (de->d_name[0] == '.') continue;
                    char full[1536];
                    snprintf(full, sizeof(full), "%s/%s", lib->target_path, de->d_name);
                    struct stat entry_st;
                    if (stat(full, &entry_st) != 0) continue;
                    if (S_ISREG(entry_st.st_mode)) {
                        if (tdvd_library_is_tdvd_file(de->d_name)) {
                            add_catalog_item(lib, full, NULL);
                        }
                    } else if (S_ISDIR(entry_st.st_mode)) {
                        if (tdvd_library_is_dvd_dir(full)) {
                            add_catalog_item(lib, full, NULL);
                        }
                    }
                }
                closedir(dir);
            }
        }
    } else {
        if (!lib->is_library) {
            add_catalog_item(lib, lib->target_path, NULL);
        }
    }

    if (lib->catalog_count > 1) {
        qsort(lib->catalog, (size_t)lib->catalog_count, sizeof(catalog_item_t), compare_catalog_items);
    }

    if (lib->catalog_count > 0 && lib->default_disc_path[0] == '\0') {
        snprintf(lib->default_disc_path, sizeof(lib->default_disc_path), "%s", lib->catalog[0].path);
        snprintf(lib->default_disc_id, sizeof(lib->default_disc_id), "%s", lib->catalog[0].id);
        snprintf(lib->default_disc_title, sizeof(lib->default_disc_title), "%s", lib->catalog[0].title);
    }
    pthread_mutex_unlock(&lib->catalog_lock);
}

void tdvd_library_init(tdvd_library_t *lib, const char *target_path) {
    if (!lib) return;
    memset(lib, 0, sizeof(*lib));
    pthread_mutex_init(&lib->catalog_lock, NULL);
    pthread_mutex_init(&lib->import_job.lock, NULL);
    lib->import_job.state = IMPORT_IDLE;

    if (target_path) {
        expand_tilde_path(target_path, lib->target_path, sizeof(lib->target_path));
    }

    struct stat st;
    if (stat(lib->target_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        lib->is_library = !tdvd_library_is_dvd_dir(lib->target_path);
    } else {
        lib->is_library = false;
    }

    tdvd_library_rescan(lib);
}

void tdvd_library_set_directory(tdvd_library_t *lib, const char *dir_path) {
    if (!lib || !dir_path || !dir_path[0]) return;
    pthread_mutex_lock(&lib->catalog_lock);
    expand_tilde_path(dir_path, lib->target_path, sizeof(lib->target_path));
    lib->is_library = true;
    pthread_mutex_unlock(&lib->catalog_lock);
    tdvd_library_rescan(lib);
}

void tdvd_library_destroy(tdvd_library_t *lib) {
    if (!lib) return;

    pthread_mutex_lock(&lib->import_job.lock);
    lib->import_job.cancel_requested = true;
    pid_t cpid = lib->import_job.child_pid;
    bool join_import = lib->import_job.thread_valid;
    pthread_t import_tid = lib->import_job.worker_thread;
    lib->import_job.thread_valid = false;
    pthread_mutex_unlock(&lib->import_job.lock);

    if (cpid > 0) {
        kill(-cpid, SIGTERM);
    }
    if (join_import) {
        pthread_join(import_tid, NULL);
    }

    if (lib->catalog) {
        free(lib->catalog);
        lib->catalog = NULL;
    }
    pthread_mutex_destroy(&lib->import_job.lock);
    pthread_mutex_destroy(&lib->catalog_lock);
}

uint8_t *tdvd_library_read_thumbnail(const char *disc_path, size_t *out_size) {
    if (out_size) *out_size = 0;
    if (!disc_path || !disc_path[0]) return NULL;

    if (tdvd_library_is_tdvd_file(disc_path)) {
        tdvd_archive_t *arc = tdvd_archive_open(disc_path);
        uint64_t toff = 0, tsz = 0;
        if (arc && tdvd_archive_find(arc, "thumbnail.png", &toff, &tsz) &&
            tsz > 0 && tsz < 16 * 1024 * 1024) {
            FILE *f = fopen(disc_path, "rb");
            if (f) {
                uint8_t *buf = (uint8_t *)malloc((size_t)tsz);
                if (buf) {
                    fseeko(f, (off_t)toff, SEEK_SET);
                    size_t n = fread(buf, 1, (size_t)tsz, f);
                    fclose(f);
                    tdvd_archive_close(arc);
                    if (n == (size_t)tsz) {
                        if (out_size) *out_size = n;
                        return buf;
                    }
                    free(buf);
                    return NULL;
                }
                fclose(f);
            }
        }
        if (arc) tdvd_archive_close(arc);
        return NULL;
    }

    struct stat st;
    if (stat(disc_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        char tpath[1536];
        snprintf(tpath, sizeof(tpath), "%s/thumbnail.png", disc_path);
        FILE *f = fopen(tpath, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            if (sz > 0 && sz < 16 * 1024 * 1024) {
                fseek(f, 0, SEEK_SET);
                uint8_t *buf = (uint8_t *)malloc((size_t)sz);
                if (buf) {
                    size_t n = fread(buf, 1, (size_t)sz, f);
                    fclose(f);
                    if (n == (size_t)sz) {
                        if (out_size) *out_size = n;
                        return buf;
                    }
                    free(buf);
                    return NULL;
                }
            }
            fclose(f);
        }
    }
    return NULL;
}

static void sanitize_user_title(const char *raw_title, char *out_title, size_t out_len) {
    if (!out_title || out_len == 0) return;
    out_title[0] = '\0';
    if (!raw_title) return;

    char tmp[256];
    size_t j = 0;
    bool prev_space = true;
    for (size_t i = 0; raw_title[i] && j + 1 < sizeof(tmp); i++) {
        unsigned char c = (unsigned char)raw_title[i];
        if (c < 32) continue;
        if (c == '/' || c == '\\' || c == ':') {
            tmp[j++] = '-';
            prev_space = false;
        } else if (isspace(c)) {
            if (!prev_space) {
                tmp[j++] = ' ';
                prev_space = true;
            }
        } else {
            tmp[j++] = (char)c;
            prev_space = false;
        }
    }
    while (j > 0 && tmp[j - 1] == ' ') j--;
    tmp[j] = '\0';
    snprintf(out_title, out_len, "%s", tmp);
}

static void clean_disc_label_to_title(const char *raw_label, char *out_title, size_t out_len) {
    if (!out_title || out_len == 0) return;
    out_title[0] = '\0';
    if (!raw_label || !raw_label[0]) return;

    char path_copy[512];
    snprintf(path_copy, sizeof(path_copy), "%s", raw_label);
    size_t plen = strlen(path_copy);
    while (plen > 1 && path_copy[plen - 1] == '/') {
        path_copy[--plen] = '\0';
    }

    const char *slash = strrchr(path_copy, '/');
    const char *base = slash ? slash + 1 : path_copy;

    if (strcasecmp(base, "VIDEO_TS") == 0 && slash && slash > path_copy) {
        *(char *)slash = '\0';
        const char *pslash = strrchr(path_copy, '/');
        base = pslash ? pslash + 1 : path_copy;
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "%s", base);
    size_t blen = strlen(buf);
    if (blen > 8 && strcasecmp(buf + blen - 8, ".tinydvd") == 0) {
        buf[blen - 8] = '\0';
    } else if (blen > 5 && strcasecmp(buf + blen - 5, ".tdvd") == 0) {
        buf[blen - 5] = '\0';
    } else if (blen > 4 && (strcasecmp(buf + blen - 4, ".iso") == 0 ||
                             strcasecmp(buf + blen - 4, ".img") == 0)) {
        buf[blen - 4] = '\0';
    }

    char cleaned[256];
    size_t j = 0;
    bool prev_space = true;
    bool has_alpha = false;
    bool has_lower = false;

    for (size_t i = 0; buf[i] && j + 1 < sizeof(cleaned); i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c < 32) continue;
        if (c == '_' || c == '.' || isspace(c)) {
            if (!prev_space) {
                cleaned[j++] = ' ';
                prev_space = true;
            }
        } else if (c == '/' || c == '\\' || c == ':') {
            cleaned[j++] = '-';
            prev_space = false;
        } else {
            if (isalpha(c)) {
                has_alpha = true;
                if (islower(c)) has_lower = true;
            }
            cleaned[j++] = (char)c;
            prev_space = false;
        }
    }
    while (j > 0 && cleaned[j - 1] == ' ') j--;
    cleaned[j] = '\0';

    if (has_alpha && !has_lower) {
        bool word_start = true;
        for (size_t i = 0; cleaned[i]; i++) {
            unsigned char c = (unsigned char)cleaned[i];
            if (isalpha(c)) {
                cleaned[i] = word_start ? (char)toupper(c) : (char)tolower(c);
                word_start = false;
            } else if (c == ' ' || c == '-') {
                word_start = true;
            } else {
                word_start = false;
            }
        }
    }

    snprintf(out_title, out_len, "%s", cleaned);
}

void tdvd_library_detect_default_source(char *out_source, size_t source_len,
                                        char *out_label, size_t label_len) {
    snprintf(out_source, source_len, "/dev/sr0");
    if (out_label && label_len > 0) out_label[0] = '\0';

#ifdef __APPLE__
    DIR *vols = opendir("/Volumes");
    if (vols) {
        struct dirent *de;
        while ((de = readdir(vols)) != NULL) {
            if (de->d_name[0] == '.') continue;
            char cand1[1024], cand2[1024];
            struct stat st;
            snprintf(cand1, sizeof(cand1), "/Volumes/%s/VIDEO_TS/VIDEO_TS.IFO", de->d_name);
            snprintf(cand2, sizeof(cand2), "/Volumes/%s/VIDEO_TS/video_ts.ifo", de->d_name);
            if (stat(cand1, &st) == 0 || stat(cand2, &st) == 0) {
                snprintf(out_source, source_len, "/Volumes/%s", de->d_name);
                if (out_label && label_len > 0) {
                    clean_disc_label_to_title(de->d_name, out_label, label_len);
                }
                closedir(vols);
                return;
            }
        }
        closedir(vols);
    }
#endif

    const char *candidates[] = {
        "/dev/sr0",
        "/dev/dvd",
        "/dev/cdrom"
#ifndef __APPLE__
        , "/dev/rdisk1", "/dev/rdisk2"
#endif
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (access(candidates[i], F_OK) == 0) {
            snprintf(out_source, source_len, "%s", candidates[i]);
            return;
        }
    }
}

static void import_append_log(import_job_t *job, const char *line) {
    if (!job || !line) return;
    char buf[IMPORT_LOG_LINE_LEN];
    snprintf(buf, sizeof(buf), "%s", line);
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }
    if (len == 0) return;

    pthread_mutex_lock(&job->lock);
    if (job->log_count >= IMPORT_MAX_LOGS) {
        memmove(job->log_lines[0], job->log_lines[1],
                (IMPORT_MAX_LOGS - 1) * IMPORT_LOG_LINE_LEN);
        job->log_count = IMPORT_MAX_LOGS - 1;
    }
    snprintf(job->log_lines[job->log_count], IMPORT_LOG_LINE_LEN, "%s", buf);
    job->log_count++;
    pthread_mutex_unlock(&job->lock);
}

static int run_import_command(import_job_t *job, char *const argv[]) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        setpgid(0, 0);
        setenv("PYTHONUNBUFFERED", "1", 1);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        if (pipefd[1] > STDERR_FILENO) {
            close(pipefd[1]);
        }
        execvp(argv[0], argv);
        fprintf(stderr, "Failed to launch '%s': %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    close(pipefd[1]);

    pthread_mutex_lock(&job->lock);
    job->child_pid = pid;
    pthread_mutex_unlock(&job->lock);

    bool sent_sigterm = false;
    char line_buf[IMPORT_LOG_LINE_LEN];
    size_t line_pos = 0;

    for (;;) {
        pthread_mutex_lock(&job->lock);
        bool cancel = job->cancel_requested;
        pthread_mutex_unlock(&job->lock);

        if (cancel && !sent_sigterm) {
            kill(-pid, SIGTERM);
            sent_sigterm = true;
        }

        struct pollfd pfd;
        pfd.fd = pipefd[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) {
            continue;
        }

        char read_buf[1024];
        ssize_t n = read(pipefd[0], read_buf, sizeof(read_buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) {
            break;
        }

        for (ssize_t i = 0; i < n; i++) {
            char c = read_buf[i];
            if (c == '\n' || c == '\r') {
                if (line_pos > 0) {
                    line_buf[line_pos] = '\0';
                    import_append_log(job, line_buf);
                    line_pos = 0;
                }
            } else {
                if (line_pos + 1 < sizeof(line_buf)) {
                    line_buf[line_pos++] = c;
                } else {
                    line_buf[line_pos] = '\0';
                    import_append_log(job, line_buf);
                    line_pos = 0;
                }
            }
        }
    }

    if (line_pos > 0) {
        line_buf[line_pos] = '\0';
        import_append_log(job, line_buf);
    }
    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            status = -1;
            break;
        }
    }

    pthread_mutex_lock(&job->lock);
    job->child_pid = 0;
    bool cancelled = job->cancel_requested;
    pthread_mutex_unlock(&job->lock);

    if (cancelled) return -2;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
    return -1;
}

static void remove_dir_recursive(const char *dir_path) {
    if (!dir_path || !dir_path[0]) return;
    struct stat st;
    if (lstat(dir_path, &st) != 0) return;

    if (!S_ISDIR(st.st_mode)) {
        unlink(dir_path);
        return;
    }

    DIR *d = opendir(dir_path);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            char child[1536];
            snprintf(child, sizeof(child), "%s/%s", dir_path, de->d_name);
            remove_dir_recursive(child);
        }
        closedir(d);
    }
    rmdir(dir_path);
}

static bool find_convert_script(char *out_path, size_t out_len) {
    const char *candidates[] = {
        "./scripts/convert-to-tinydvd.py",
        "../scripts/convert-to-tinydvd.py",
        "./build/convert-to-tinydvd",
        "/usr/local/bin/convert-to-tinydvd",
        "/usr/bin/convert-to-tinydvd"
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (access(candidates[i], R_OK) == 0) {
            snprintf(out_path, out_len, "%s", candidates[i]);
            return true;
        }
    }
    return false;
}

static void import_set_error(import_job_t *job, const char *msg) {
    pthread_mutex_lock(&job->lock);
    job->state = IMPORT_ERROR;
    job->active = false;
    snprintf(job->stage_msg, sizeof(job->stage_msg), "Import failed");
    snprintf(job->error_msg, sizeof(job->error_msg), "%s", msg ? msg : "Unknown error");
    pthread_mutex_unlock(&job->lock);
}

static void *import_worker_thread(void *arg) {
    tdvd_library_t *lib = (tdvd_library_t *)arg;
    import_job_t *job = &lib->import_job;

    char source[512];
    char user_title[256];
    bool skip_enrichment = false;
    pthread_mutex_lock(&job->lock);
    snprintf(source, sizeof(source), "%s", job->source);
    snprintf(user_title, sizeof(user_title), "%s", job->title);
    skip_enrichment = job->skip_enrichment;
    pthread_mutex_unlock(&job->lock);

    char library_dir[1024];
    pthread_mutex_lock(&lib->catalog_lock);
    if (lib->is_library) {
        snprintf(library_dir, sizeof(library_dir), "%s", lib->target_path);
    } else {
        char copy[1024];
        snprintf(copy, sizeof(copy), "%s", lib->target_path);
        char *slash = strrchr(copy, '/');
        if (slash && slash > copy) {
            *slash = '\0';
            snprintf(library_dir, sizeof(library_dir), "%s", copy);
        } else if (slash == copy) {
            snprintf(library_dir, sizeof(library_dir), "/");
        } else {
            snprintf(library_dir, sizeof(library_dir), ".");
        }
        lib->is_library = true;
        snprintf(lib->target_path, sizeof(lib->target_path), "%s", library_dir);
    }
    pthread_mutex_unlock(&lib->catalog_lock);

    struct stat st;
    bool is_device = (strncmp(source, "/dev/", 5) == 0);
    if (!is_device && stat(source, &st) == 0) {
        if (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)) {
            is_device = true;
        }
    }

    char convert_input[1536] = {0};
    char temp_rip_dir[1280] = {0};
    bool has_temp_rip = false;
    char final_title[256] = {0};

    sanitize_user_title(user_title, final_title, sizeof(final_title));

    if (is_device) {
        if (access(source, F_OK) != 0) {
            import_set_error(job, "Optical drive device not found. Check the drive path or choose a folder/ISO.");
            return NULL;
        }

        pthread_mutex_lock(&job->lock);
        job->state = IMPORT_RIPPING;
        snprintf(job->stage_msg, sizeof(job->stage_msg), "Backing up DVD from optical drive (dvdbackup)...");
        pthread_mutex_unlock(&job->lock);

        snprintf(temp_rip_dir, sizeof(temp_rip_dir), "%s/.tdvd_rip_%d_%ld",
                 library_dir, (int)getpid(), (long)time(NULL));
        if (mkdir(temp_rip_dir, 0755) != 0) {
            import_set_error(job, "Failed to create temporary rip directory inside library folder.");
            return NULL;
        }
        has_temp_rip = true;

        import_append_log(job, ">>> Starting dvdbackup mirror from optical drive...");
        char *rip_argv[] = { "dvdbackup", "-M", "-i", source, "-o", temp_rip_dir, NULL };
        int rc = run_import_command(job, rip_argv);
        if (rc == -2) {
            remove_dir_recursive(temp_rip_dir);
            import_set_error(job, "Import cancelled by user.");
            return NULL;
        }
        if (rc != 0) {
            remove_dir_recursive(temp_rip_dir);
            import_set_error(job, "dvdbackup failed (ensure a DVD is inserted and dvdbackup + libdvdcss are installed).");
            return NULL;
        }

        char ripped_sub[1536] = {0};
        char disc_label[256] = {0};
        DIR *rd = opendir(temp_rip_dir);
        if (rd) {
            struct dirent *de;
            while ((de = readdir(rd)) != NULL) {
                if (de->d_name[0] == '.') continue;
                char cand[1536];
                snprintf(cand, sizeof(cand), "%s/%s", temp_rip_dir, de->d_name);
                struct stat cst;
                if (stat(cand, &cst) == 0 && S_ISDIR(cst.st_mode)) {
                    snprintf(ripped_sub, sizeof(ripped_sub), "%s", cand);
                    snprintf(disc_label, sizeof(disc_label), "%s", de->d_name);
                    break;
                }
            }
            closedir(rd);
        }

        if (ripped_sub[0]) {
            snprintf(convert_input, sizeof(convert_input), "%s", ripped_sub);
            if (!final_title[0]) {
                clean_disc_label_to_title(disc_label, final_title, sizeof(final_title));
            }
        } else {
            snprintf(convert_input, sizeof(convert_input), "%s", temp_rip_dir);
        }
    } else {
        if (stat(source, &st) != 0) {
            import_set_error(job, "Source path not found.");
            return NULL;
        }
        snprintf(convert_input, sizeof(convert_input), "%s", source);
        if (!final_title[0]) {
            clean_disc_label_to_title(source, final_title, sizeof(final_title));
        }
    }

    if (!final_title[0]) {
        snprintf(final_title, sizeof(final_title), "Imported DVD %ld", (long)time(NULL));
    }

    size_t flen = strlen(final_title);
    if (flen > 5 && strcasecmp(final_title + flen - 5, ".tdvd") == 0) {
        final_title[flen - 5] = '\0';
    } else if (flen > 8 && strcasecmp(final_title + flen - 8, ".tinydvd") == 0) {
        final_title[flen - 8] = '\0';
    }

    pthread_mutex_lock(&job->lock);
    if (job->cancel_requested) {
        pthread_mutex_unlock(&job->lock);
        if (has_temp_rip) remove_dir_recursive(temp_rip_dir);
        import_set_error(job, "Import cancelled by user.");
        return NULL;
    }
    job->state = IMPORT_CONVERTING;
    snprintf(job->stage_msg, sizeof(job->stage_msg), "Converting to TinyDVD (.tdvd)%s...",
             skip_enrichment ? "" : " & enriching metadata");
    pthread_mutex_unlock(&job->lock);

    char filename[300];
    snprintf(filename, sizeof(filename), "%s.tdvd", final_title);
    char output_tdvd[1536];
    snprintf(output_tdvd, sizeof(output_tdvd), "%s/%s", library_dir, filename);

    import_append_log(job, ">>> Running convert-to-tinydvd...");

    char script_path[512];
    int conv_rc = -1;
    if (find_convert_script(script_path, sizeof(script_path))) {
        if (skip_enrichment) {
            char *argv[] = { "python3", "-u", script_path, convert_input, output_tdvd, "--skip-enrichment", NULL };
            conv_rc = run_import_command(job, argv);
        } else {
            char *argv[] = { "python3", "-u", script_path, convert_input, output_tdvd, NULL };
            conv_rc = run_import_command(job, argv);
        }
    } else {
        if (skip_enrichment) {
            char *argv[] = { "convert-to-tinydvd", convert_input, output_tdvd, "--skip-enrichment", NULL };
            conv_rc = run_import_command(job, argv);
        } else {
            char *argv[] = { "convert-to-tinydvd", convert_input, output_tdvd, NULL };
            conv_rc = run_import_command(job, argv);
        }
    }

    if (has_temp_rip) {
        import_append_log(job, ">>> Cleaning up temporary DVD rip folder...");
        remove_dir_recursive(temp_rip_dir);
    }

    if (conv_rc == -2) {
        import_set_error(job, "Import cancelled by user.");
        return NULL;
    }
    if (conv_rc != 0) {
        import_set_error(job, "Conversion to TinyDVD (.tdvd) failed. Check the progress log for details.");
        return NULL;
    }

    /* Check if convert-to-tinydvd disambiguated the archive filename (year or numeric suffix) */
    const char *final_marker = "[output] Final archive: ";
    size_t marker_len = strlen(final_marker);
    pthread_mutex_lock(&job->lock);
    for (int i = job->log_count - 1; i >= 0; i--) {
        if (strncmp(job->log_lines[i], final_marker, marker_len) == 0) {
            const char *actual = job->log_lines[i] + marker_len;
            if (actual[0]) {
                snprintf(output_tdvd, sizeof(output_tdvd), "%s", actual);
                const char *sl = strrchr(output_tdvd, '/');
                const char *bn = sl ? (sl + 1) : output_tdvd;
                snprintf(filename, sizeof(filename), "%s", bn);
            }
            break;
        }
    }
    pthread_mutex_unlock(&job->lock);

    if (stat(output_tdvd, &st) != 0 || st.st_size == 0) {
        import_set_error(job, "Conversion finished but output .tdvd archive was not found.");
        return NULL;
    }

    tdvd_library_rescan(lib);

    char disc_id[128] = {0};
    tdvd_library_sanitize_id(filename, disc_id, sizeof(disc_id));

    char res_title[256] = {0};
    snprintf(res_title, sizeof(res_title), "%s", final_title);
    pthread_mutex_lock(&lib->catalog_lock);
    for (int i = 0; i < lib->catalog_count; i++) {
        if (strcmp(lib->catalog[i].id, disc_id) == 0 && lib->catalog[i].title[0]) {
            snprintf(res_title, sizeof(res_title), "%s", lib->catalog[i].title);
            break;
        }
    }
    pthread_mutex_unlock(&lib->catalog_lock);

    pthread_mutex_lock(&job->lock);
    job->state = IMPORT_DONE;
    job->active = false;
    snprintf(job->result_disc_id, sizeof(job->result_disc_id), "%s", disc_id);
    snprintf(job->result_disc_path, sizeof(job->result_disc_path), "%s", output_tdvd);
    snprintf(job->result_title, sizeof(job->result_title), "%s", res_title);
    snprintf(job->stage_msg, sizeof(job->stage_msg), "Import complete!");
    job->error_msg[0] = '\0';
    pthread_mutex_unlock(&job->lock);

    fprintf(stderr, "[library] DVD import completed: %s -> %s\n", res_title, output_tdvd);
    return NULL;
}

bool tdvd_library_import_start(tdvd_library_t *lib, const char *source, const char *title,
                               bool skip_enrichment, char *err_buf, size_t err_len) {
    if (!lib) return false;
    if (err_buf && err_len > 0) err_buf[0] = '\0';

    char src_buf[512] = {0};
    if (source && source[0]) {
        expand_tilde_path(source, src_buf, sizeof(src_buf));
    } else {
        char dummy_label[256] = {0};
        tdvd_library_detect_default_source(src_buf, sizeof(src_buf), dummy_label, sizeof(dummy_label));
    }

    import_job_t *job = &lib->import_job;
    pthread_mutex_lock(&job->lock);
    if (job->active) {
        pthread_mutex_unlock(&job->lock);
        if (err_buf && err_len > 0) {
            snprintf(err_buf, err_len, "An import job is already in progress");
        }
        return false;
    }

    pthread_t old_thread;
    bool need_join = job->thread_valid;
    if (need_join) {
        old_thread = job->worker_thread;
        job->thread_valid = false;
    }
    pthread_mutex_unlock(&job->lock);

    if (need_join) {
        pthread_join(old_thread, NULL);
    }

    pthread_mutex_lock(&job->lock);
    if (job->active) {
        pthread_mutex_unlock(&job->lock);
        if (err_buf && err_len > 0) {
            snprintf(err_buf, err_len, "An import job is already in progress");
        }
        return false;
    }

    job->state = IMPORT_CONVERTING;
    job->active = true;
    job->cancel_requested = false;
    job->child_pid = 0;
    job->start_time = time(NULL);
    snprintf(job->source, sizeof(job->source), "%s", src_buf);
    snprintf(job->title, sizeof(job->title), "%s", (title && title[0]) ? title : "");
    job->skip_enrichment = skip_enrichment;
    snprintf(job->stage_msg, sizeof(job->stage_msg), "Starting DVD import...");
    job->error_msg[0] = '\0';
    job->result_disc_id[0] = '\0';
    job->result_disc_path[0] = '\0';
    job->result_title[0] = '\0';
    job->log_count = 0;

    if (pthread_create(&job->worker_thread, NULL, import_worker_thread, lib) == 0) {
        job->thread_valid = true;
        pthread_mutex_unlock(&job->lock);
        return true;
    }

    job->active = false;
    job->state = IMPORT_ERROR;
    snprintf(job->error_msg, sizeof(job->error_msg), "Failed to spawn import worker thread");
    pthread_mutex_unlock(&job->lock);
    if (err_buf && err_len > 0) {
        snprintf(err_buf, err_len, "Failed to start import thread");
    }
    return false;
}

void tdvd_library_import_cancel(tdvd_library_t *lib) {
    if (!lib) return;
    import_job_t *job = &lib->import_job;
    pthread_mutex_lock(&job->lock);
    job->cancel_requested = true;
    pid_t cpid = job->child_pid;
    pthread_mutex_unlock(&job->lock);

    if (cpid > 0) {
        kill(-cpid, SIGTERM);
    }
}

const char *tdvd_import_state_name(import_state_t st) {
    switch (st) {
        case IMPORT_IDLE:       return "idle";
        case IMPORT_RIPPING:    return "ripping";
        case IMPORT_CONVERTING: return "converting";
        case IMPORT_DONE:       return "done";
        case IMPORT_ERROR:      return "error";
        default:                return "idle";
    }
}
