/*
 * The executable inside TinyDVD.app on macOS.
 *
 * The command-line `tinydvd` launcher execs a Python interpreter. On macOS
 * that would make the running process Homebrew's Python.app, so the Dock
 * would show "Python", and Finder's "open this document" requests would go
 * to the wrong app. This launcher runs Python inside its own process
 * instead, so the process stays TinyDVD.app: TinyDVD's name and icon, and
 * .tdvd files double-clicked in Finder arrive as SDL drop events.
 *
 * It runs the installed runtime in TINYDVD_INSTALL_LIBDIR, the same files
 * the command-line launcher uses.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef TINYDVD_INSTALL_LIBDIR
#define TINYDVD_INSTALL_LIBDIR "/usr/local/lib/tinydvd"
#endif

/* Apps opened from Finder get a minimal PATH; add Homebrew's, where ffmpeg
 * (used for thumbnails and importing) lives */
static void add_homebrew_to_path(void) {
    static const char *dirs[] = { "/opt/homebrew/bin", "/usr/local/bin" };
    const char *old = getenv("PATH");
    char path[8192];
    snprintf(path, sizeof(path), "%s", (old && old[0]) ? old : "/usr/bin:/bin:/usr/sbin:/sbin");
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        struct stat st;
        if (stat(dirs[i], &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        char needle[PATH_MAX + 2];
        snprintf(needle, sizeof(needle), ":%s:", dirs[i]);
        char padded[8200];
        snprintf(padded, sizeof(padded), ":%s:", path);
        if (strstr(padded, needle)) continue;
        char updated[8192];
        snprintf(updated, sizeof(updated), "%s:%s", dirs[i], path);
        snprintf(path, sizeof(path), "%s", updated);
    }
    setenv("PATH", path, 1);
}

int main(int argc, char *argv[]) {
    const char *libdir = TINYDVD_INSTALL_LIBDIR;
    char script[PATH_MAX];
    char core_lib[PATH_MAX];
    char python[PATH_MAX];
    snprintf(script, sizeof(script), "%s/scripts/tinydvd_app.py", libdir);
    snprintf(core_lib, sizeof(core_lib), "%s/libtinydvd_core.dylib", libdir);
    snprintf(python, sizeof(python), "%s/venv/bin/python3", libdir);

    setenv("TINYDVD_CORE_LIB", core_lib, 0);
    setenv("TINYDVD_PROG", "TinyDVD", 1);
    add_homebrew_to_path();

    /* Python's argv: the interpreter, the app script, then our arguments,
     * minus the "-psn_..." process serial number older macOS versions add */
    char **py_argv = (char **)calloc((size_t)argc + 2, sizeof(char *));
    if (!py_argv) return 1;
    int py_argc = 0;
    py_argv[py_argc++] = python;
    py_argv[py_argc++] = script;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-psn_", 5) != 0) {
            py_argv[py_argc++] = argv[i];
        }
    }

    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    /* sys.executable is the real interpreter, so tools the app runs as
     * subprocesses (the converter, the thumbnailer) start Python, not
     * another copy of this app */
    PyStatus status = PyConfig_SetBytesString(&config, &config.executable, python);
    if (!PyStatus_Exception(status)) {
        status = PyConfig_SetBytesArgv(&config, py_argc, py_argv);
    }
    if (!PyStatus_Exception(status)) {
        status = Py_InitializeFromConfig(&config);
    }
    PyConfig_Clear(&config);
    free(py_argv);
    if (PyStatus_Exception(status)) {
        if (PyStatus_IsExit(status)) {
            return status.exitcode;
        }
        Py_ExitStatusException(status);
    }
    return Py_RunMain();
}
