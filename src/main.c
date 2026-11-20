#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifndef PYTHON3_EXECUTABLE
#define PYTHON3_EXECUTABLE "python3"
#endif

#ifndef TINYDVD_SOURCE_DIR
#define TINYDVD_SOURCE_DIR "."
#endif

#ifndef TINYDVD_CORE_LIB
#define TINYDVD_CORE_LIB ""
#endif

#ifndef TINYDVD_INSTALL_LIBDIR
#define TINYDVD_INSTALL_LIBDIR "/usr/local/lib/tinydvd"
#endif

#ifdef __APPLE__
#define CORE_LIB_NAME "libtinydvd_core.dylib"
#else
#define CORE_LIB_NAME "libtinydvd_core.so"
#endif

static bool resolve_exe_dir(char *out_dir, size_t out_sz) {
    char raw_path[4096];
    raw_path[0] = '\0';

#ifdef __APPLE__
    uint32_t sz = (uint32_t)sizeof(raw_path);
    if (_NSGetExecutablePath(raw_path, &sz) != 0) {
        return false;
    }
#else
    ssize_t n = readlink("/proc/self/exe", raw_path, sizeof(raw_path) - 1);
    if (n <= 0) {
        return false;
    }
    raw_path[n] = '\0';
#endif

    char resolved[4096];
    const char *src = raw_path;
    if (realpath(raw_path, resolved) != NULL) {
        src = resolved;
    }

    snprintf(out_dir, out_sz, "%s", src);
    char *slash = strrchr(out_dir, '/');
    if (!slash) {
        return false;
    }
    if (slash == out_dir) {
        out_dir[1] = '\0';
    } else {
        *slash = '\0';
    }
    return true;
}

static bool file_exists(const char *path) {
    return path && path[0] && access(path, F_OK) == 0;
}

static bool file_readable(const char *path) {
    return path && path[0] && access(path, R_OK) == 0;
}

static bool file_executable(const char *path) {
    return path && path[0] && access(path, X_OK) == 0;
}

static const char *pick_python(const char *venv_py, char *out_buf, size_t out_sz) {
    const char *env_py = getenv("TINYDVD_PYTHON");
    if (file_executable(env_py)) {
        snprintf(out_buf, out_sz, "%s", env_py);
        return out_buf;
    }
    if (file_executable(venv_py)) {
        snprintf(out_buf, out_sz, "%s", venv_py);
        return out_buf;
    }
    if (file_executable(PYTHON3_EXECUTABLE)) {
        snprintf(out_buf, out_sz, "%s", PYTHON3_EXECUTABLE);
        return out_buf;
    }
    snprintf(out_buf, out_sz, "%s", "python3");
    return out_buf;
}

int main(int argc, char *argv[]) {
    if (argc > 0 && argv[0]) {
        setenv("TINYDVD_PROG", argv[0], 1);
    }

    char exe_dir[2048];
    bool has_exe_dir = resolve_exe_dir(exe_dir, sizeof(exe_dir));

    char chosen_lib[4096];
    char chosen_script[4096];
    char chosen_py[4096];
    chosen_lib[0] = '\0';
    chosen_script[0] = '\0';
    chosen_py[0] = '\0';

    bool found = false;

    /* 1. Local build tree (<build>/tinydvd + <build>/libtinydvd_core + <src>/scripts/tinydvd_app.py) */
    if (has_exe_dir) {
        char cand_lib[4096];
        char cand_script[4096];
        snprintf(cand_lib, sizeof(cand_lib), "%s/%s", exe_dir, CORE_LIB_NAME);
        snprintf(cand_script, sizeof(cand_script), "%s/scripts/tinydvd_app.py", TINYDVD_SOURCE_DIR);
        if (file_exists(cand_lib) && file_readable(cand_script)) {
            snprintf(chosen_lib, sizeof(chosen_lib), "%s", cand_lib);
            snprintf(chosen_script, sizeof(chosen_script), "%s", cand_script);
            pick_python(NULL, chosen_py, sizeof(chosen_py));
            found = true;
        }
    }

    /* 2. Relocatable installed prefix (<prefix>/bin/tinydvd -> <prefix>/lib/tinydvd/...) */
    if (!found && has_exe_dir) {
        char prefix_dir[2048];
        snprintf(prefix_dir, sizeof(prefix_dir), "%s", exe_dir);
        char *slash = strrchr(prefix_dir, '/');
        if (slash) {
            if (slash == prefix_dir) prefix_dir[1] = '\0';
            else *slash = '\0';

            char cand_lib[4096];
            char cand_script[4096];
            char cand_venv_py[4096];
            const char *sep = (strcmp(prefix_dir, "/") == 0) ? "" : prefix_dir;
            snprintf(cand_lib, sizeof(cand_lib), "%s/lib/tinydvd/%s", sep, CORE_LIB_NAME);
            snprintf(cand_script, sizeof(cand_script), "%s/lib/tinydvd/scripts/tinydvd_app.py", sep);
            snprintf(cand_venv_py, sizeof(cand_venv_py), "%s/lib/tinydvd/venv/bin/python3", sep);
            if (file_exists(cand_lib) && file_readable(cand_script)) {
                snprintf(chosen_lib, sizeof(chosen_lib), "%s", cand_lib);
                snprintf(chosen_script, sizeof(chosen_script), "%s", cand_script);
                pick_python(cand_venv_py, chosen_py, sizeof(chosen_py));
                found = true;
            }
        }
    }

    /* 3. Configured install libdir (TINYDVD_INSTALL_LIBDIR) */
    const char *install_libdir = TINYDVD_INSTALL_LIBDIR;
    if (!found && install_libdir[0]) {
        char cand_lib[4096];
        char cand_script[4096];
        char cand_venv_py[4096];
        snprintf(cand_lib, sizeof(cand_lib), "%s/%s", install_libdir, CORE_LIB_NAME);
        snprintf(cand_script, sizeof(cand_script), "%s/scripts/tinydvd_app.py", install_libdir);
        snprintf(cand_venv_py, sizeof(cand_venv_py), "%s/venv/bin/python3", install_libdir);
        if (file_exists(cand_lib) && file_readable(cand_script)) {
            snprintf(chosen_lib, sizeof(chosen_lib), "%s", cand_lib);
            snprintf(chosen_script, sizeof(chosen_script), "%s", cand_script);
            pick_python(cand_venv_py, chosen_py, sizeof(chosen_py));
            found = true;
        }
    }

    /* 4. Fallback to compile-time defaults */
    if (!found) {
        snprintf(chosen_lib, sizeof(chosen_lib), "%s", TINYDVD_CORE_LIB);
        snprintf(chosen_script, sizeof(chosen_script), "%s/scripts/tinydvd_app.py", TINYDVD_SOURCE_DIR);
        pick_python(NULL, chosen_py, sizeof(chosen_py));
    }

    if (chosen_lib[0]) {
        setenv("TINYDVD_CORE_LIB", chosen_lib, 0);
    }

    char **py_argv = (char **)calloc((size_t)(argc + 2), sizeof(char *));
    if (!py_argv) return 1;
    py_argv[0] = chosen_py;
    py_argv[1] = chosen_script;
    for (int i = 1; i < argc; i++) {
        py_argv[i + 1] = argv[i];
    }
    py_argv[argc + 1] = NULL;

    execvp(chosen_py, py_argv);
    perror("execvp python3");
    free(py_argv);
    return 1;
}
