#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifndef PYTHON3_EXECUTABLE
#define PYTHON3_EXECUTABLE "python3"
#endif

#ifndef TINYDVD_SOURCE_DIR
#define TINYDVD_SOURCE_DIR "."
#endif

#ifndef TINYDVD_CORE_LIB
#define TINYDVD_CORE_LIB ""
#endif

int main(int argc, char *argv[]) {
    if (TINYDVD_CORE_LIB[0]) {
        setenv("TINYDVD_CORE_LIB", TINYDVD_CORE_LIB, 0);
    }
    if (argc > 0 && argv[0]) {
        setenv("TINYDVD_PROG", argv[0], 1);
    }

    char script_path[2048];
    snprintf(script_path, sizeof(script_path), "%s/scripts/tinydvd_app.py", TINYDVD_SOURCE_DIR);

    char **py_argv = (char **)calloc((size_t)(argc + 2), sizeof(char *));
    if (!py_argv) return 1;
    py_argv[0] = PYTHON3_EXECUTABLE;
    py_argv[1] = script_path;
    for (int i = 1; i < argc; i++) {
        py_argv[i + 1] = argv[i];
    }
    py_argv[argc + 1] = NULL;

    execvp(PYTHON3_EXECUTABLE, py_argv);
    perror("execvp python3");
    free(py_argv);
    return 1;
}
