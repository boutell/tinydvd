#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct web_server web_server_t;

typedef struct {
    const char *bind_addr;    /* IP to bind to, default "0.0.0.0" */
    int port;                 /* Port to listen on, default 8080 */
    const char *target_path;  /* Path to .tdvd file or directory containing .tdvd files */
} web_server_config_t;

/**
 * Initialize and start the TinyDVD embedded web streaming server.
 * Returns NULL on failure.
 */
web_server_t *web_server_start(const web_server_config_t *config);

/**
 * Run web server loop (blocks until stopped or interrupted).
 */
void web_server_run(web_server_t *server);

/**
 * Signal the web server to stop.
 */
void web_server_stop(web_server_t *server);

/**
 * Clean up and destroy the web server instance.
 */
void web_server_destroy(web_server_t *server);

/**
 * Handle a connected client socket directly (e.g. for testing via socketpair).
 */
void web_server_handle_client(web_server_t *server, int client_fd);

#ifdef __cplusplus
}
#endif

#endif /* WEB_SERVER_H */
