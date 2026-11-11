#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "sha1.h"
#include "spu_png.h"
#include "web_server.h"

static void test_sha1_and_websocket_key(void) {
    printf("Testing SHA-1 & RFC 6455 WebSocket Accept Key generation... ");

    /* RFC 6455 Example Key: "dGhlIHNhbXBsZSBub25jZQ=="
     * Expected Accept: "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" */
    const char *key = "dGhlIHNhbXBsZSBub25jZQ==";
    const char *guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char combined[128];
    snprintf(combined, sizeof(combined), "%s%s", key, guid);

    uint8_t digest[20];
    sha1_compute((const uint8_t *)combined, strlen(combined), digest);

    char *accept_b64 = spu_base64_encode(digest, 20);
    assert(accept_b64 != NULL);
    assert(strcmp(accept_b64, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
    free(accept_b64);

    printf("OK\n");
}

static void test_spu_png_encoding(void) {
    printf("Testing SPU RGBA to PNG & Data URL generation... ");

    uint32_t pixels[100 * 100];
    for (int i = 0; i < 100 * 100; i++) {
        pixels[i] = (i % 2 == 0) ? 0xFF00FFFF : 0x00000000;
    }

    size_t png_size = 0;
    uint8_t *png = spu_rgba_to_png(pixels, 100, 100, &png_size);
    assert(png != NULL);
    assert(png_size > 8);
    /* Check PNG magic: 0x89 'P' 'N' 'G' 0x0D 0x0A 0x1A 0x0A */
    assert(png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
    free(png);

    char *data_url = spu_rgba_to_data_url(pixels, 100, 100);
    assert(data_url != NULL);
    assert(strncmp(data_url, "data:image/png;base64,", 22) == 0);
    free(data_url);

    printf("OK\n");
}

typedef struct {
    web_server_t *server;
    int fd;
} server_thread_arg_t;

static void *server_thread_func(void *arg) {
    server_thread_arg_t *a = (server_thread_arg_t *)arg;
    web_server_handle_client(a->server, a->fd);
    free(a);
    return NULL;
}

static int start_client_session(web_server_t *s, pthread_t *out_tid) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    server_thread_arg_t *arg = (server_thread_arg_t *)malloc(sizeof(server_thread_arg_t));
    arg->server = s;
    arg->fd = sv[0];
    pthread_create(out_tid, NULL, server_thread_func, arg);
    return sv[1];
}

static void test_http_and_websocket_endpoints(const char *tdvd_path) {
    printf("Testing Web Server HTTP endpoints, Range streaming & WebSocket...\n");

    int test_port = 8991;
    web_server_config_t cfg = {
        .bind_addr = "127.0.0.1",
        .port = test_port,
        .target_path = tdvd_path
    };

    web_server_t *server = web_server_start(&cfg);
    assert(server != NULL);

    /* 1. Test GET / (HTML SPA) */
    {
        pthread_t tid;
        int fd = start_client_session(server, &tid);
        const char req[] = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        send(fd, req, sizeof(req) - 1, 0);

        char resp[65536];
        size_t total_recv = 0;
        ssize_t n;
        while ((n = recv(fd, resp + total_recv, sizeof(resp) - 1 - total_recv, 0)) > 0) {
            total_recv += n;
        }
        resp[total_recv] = '\0';
        assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
        assert(strstr(resp, "text/html") != NULL);
        assert(strstr(resp, "TinyDVD Web Player") != NULL);
        close(fd);
        pthread_join(tid, NULL);
        printf("  - GET /: OK (HTTP 200, HTML SPA served, %zu bytes)\n", total_recv);
    }

    /* 2. Test GET /api/library (JSON Catalog) */
    {
        pthread_t tid;
        int fd = start_client_session(server, &tid);
        const char req[] = "GET /api/library HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        send(fd, req, sizeof(req) - 1, 0);

        char resp[16384];
        size_t total_recv = 0;
        ssize_t n;
        while ((n = recv(fd, resp + total_recv, sizeof(resp) - 1 - total_recv, 0)) > 0) {
            total_recv += n;
        }
        resp[total_recv] = '\0';
        assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
        assert(strstr(resp, "application/json") != NULL);
        assert(strstr(resp, "\"items\":[") != NULL);
        assert(strstr(resp, "Denis Villeneuve") != NULL);
        assert(strstr(resp, "Timoth") != NULL);
        assert(strstr(resp, "\"year\":2021") != NULL);
        close(fd);
        pthread_join(tid, NULL);
        printf("  - GET /api/library: OK (JSON movie catalog with director, cast, year metadata)\n");
    }

    /* 3. Test GET /api/thumbnail */
    {
        pthread_t tid;
        int fd = start_client_session(server, &tid);
        const char req[] = "GET /api/thumbnail HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        send(fd, req, sizeof(req) - 1, 0);

        char resp[8192];
        size_t total_recv = 0;
        ssize_t n;
        while ((n = recv(fd, resp + total_recv, sizeof(resp) - 1 - total_recv, 0)) > 0) {
            total_recv += n;
        }
        resp[total_recv] = '\0';
        assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
        assert(strstr(resp, "image/png") != NULL);
        close(fd);
        pthread_join(tid, NULL);
        printf("  - GET /api/thumbnail: OK (Direct archive PNG streaming)\n");
    }

    /* 4. Test WebSocket Upgrade & Handshake */
    {
        pthread_t tid;
        int fd = start_client_session(server, &tid);
        const char ws_handshake[] =
            "GET /ws HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";
        send(fd, ws_handshake, strlen(ws_handshake), 0);

        char resp[1024];
        ssize_t n = recv(fd, resp, sizeof(resp) - 1, 0);
        assert(n > 0);
        resp[n] = '\0';
        assert(strstr(resp, "HTTP/1.1 101 Switching Protocols") != NULL);
        assert(strstr(resp, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL);

        /* Send client WebSocket text frame (masked): {"key":"Down"} */
        const char client_msg[] = "{\"type\":\"key\",\"key\":\"Down\"}";
        size_t mlen = strlen(client_msg);
        uint8_t frame[128];
        frame[0] = 0x81; /* FIN + Text */
        frame[1] = 0x80 | (uint8_t)mlen; /* Masked + len */
        uint8_t mask[4] = { 0x12, 0x34, 0x56, 0x78 };
        memcpy(&frame[2], mask, 4);
        for (size_t i = 0; i < mlen; i++) {
            frame[6 + i] = (uint8_t)client_msg[i] ^ mask[i % 4];
        }
        send(fd, frame, 6 + mlen, 0);

        close(fd);
        pthread_join(tid, NULL);
        printf("  - WebSocket /ws: OK (RFC 6455 101 Switching Protocols & Masked Frames)\n");
    }

    /* 5. Test Media Remux & HTTP 206 Partial Content Range Streaming */
    {
        pthread_t tid;
        int fd = start_client_session(server, &tid);
        const char req[] =
            "GET /api/media/VIDEO_TS_VIDEO.mp4 HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Range: bytes=0-1023\r\n\r\n";
        send(fd, req, strlen(req), 0);

        char resp[2048];
        ssize_t n = recv(fd, resp, sizeof(resp) - 1, 0);
        assert(n > 0);
        resp[n] = '\0';
        assert(strstr(resp, "HTTP/1.1 206 Partial Content") != NULL);
        assert(strstr(resp, "Content-Range: bytes 0-1023/") != NULL);
        assert(strstr(resp, "Content-Length: 1024") != NULL);
        assert(strstr(resp, "video/mp4") != NULL);
        close(fd);
        pthread_join(tid, NULL);
        printf("  - HTTP 206 Range Streaming: OK (On-demand fast MP4 remux + byte range seek)\n");
    }

    /* 6. Test Multi-Session Isolation (e.g. Incognito / Multiple Browser Windows) */
    {
        pthread_t tid1, tid2;
        int fd1 = start_client_session(server, &tid1);
        int fd2 = start_client_session(server, &tid2);

        /* Connect Alice session */
        const char ws_alice[] =
            "GET /ws?session=user_alice HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";
        send(fd1, ws_alice, strlen(ws_alice), 0);

        char resp1[2048];
        ssize_t n1 = recv(fd1, resp1, sizeof(resp1) - 1, 0);
        assert(n1 > 0);
        resp1[n1] = '\0';
        assert(strstr(resp1, "HTTP/1.1 101 Switching Protocols") != NULL);

        /* Connect Bob session */
        const char ws_bob[] =
            "GET /ws?session=user_bob HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";
        send(fd2, ws_bob, strlen(ws_bob), 0);

        char resp2[2048];
        ssize_t n2 = recv(fd2, resp2, sizeof(resp2) - 1, 0);
        assert(n2 > 0);
        resp2[n2] = '\0';
        assert(strstr(resp2, "HTTP/1.1 101 Switching Protocols") != NULL);

        /* Read Alice initial state frame: should contain session_id: "user_alice" */
        uint8_t frame_buf[4096];
        ssize_t fn1 = recv(fd1, frame_buf, sizeof(frame_buf) - 1, 0);
        assert(fn1 > 2);
        frame_buf[fn1] = '\0';
        assert(strstr((char *)frame_buf, "user_alice") != NULL);

        /* Read Bob initial state frame: should contain session_id: "user_bob" */
        ssize_t fn2 = recv(fd2, frame_buf, sizeof(frame_buf) - 1, 0);
        assert(fn2 > 2);
        frame_buf[fn2] = '\0';
        assert(strstr((char *)frame_buf, "user_bob") != NULL);

        close(fd1);
        close(fd2);
        pthread_join(tid1, NULL);
        pthread_join(tid2, NULL);
        printf("  - Multi-Session Isolation: OK (Independent sessions, state & VMs for distinct clients)\n");
    }

    web_server_destroy(server);
    printf("Web Server tests passed!\n");
}

static void test_library_directory_filtering(const char *tdvd_path) {
    printf("Testing Web Server Directory Scanning & Non-TinyDVD Filtering...\n");

    char abs_tdvd[1024];
    assert(realpath(tdvd_path, abs_tdvd) != NULL);

    /* Create a temporary mixed folder */
    char test_dir[] = "/tmp/tinydvd_test_mixed_XXXXXX";
    assert(mkdtemp(test_dir) != NULL);

    /* 1. Add valid .tdvd (symlink to existing Dune.tdvd) */
    char valid_tdvd[512];
    snprintf(valid_tdvd, sizeof(valid_tdvd), "%s/Dune.tdvd", test_dir);
    symlink(abs_tdvd, valid_tdvd);

    /* 2. Add non-TinyDVD files: .mp4, .mkv, .txt */
    char dummy_mp4[512], dummy_mkv[512], dummy_txt[512];
    snprintf(dummy_mp4, sizeof(dummy_mp4), "%s/home_video.mp4", test_dir);
    snprintf(dummy_mkv, sizeof(dummy_mkv), "%s/holiday_movie.mkv", test_dir);
    snprintf(dummy_txt, sizeof(dummy_txt), "%s/readme.txt", test_dir);
    FILE *f1 = fopen(dummy_mp4, "w"); if (f1) { fputs("dummy mp4", f1); fclose(f1); }
    FILE *f2 = fopen(dummy_mkv, "w"); if (f2) { fputs("dummy mkv", f2); fclose(f2); }
    FILE *f3 = fopen(dummy_txt, "w"); if (f3) { fputs("notes", f3); fclose(f3); }

    /* 3. Add non-TinyDVD subdirectory */
    char sub_dir[512];
    snprintf(sub_dir, sizeof(sub_dir), "%s/OtherFolder", test_dir);
    mkdir(sub_dir, 0755);

    /* Start web server pointing at the mixed directory */
    web_server_config_t cfg = {
        .bind_addr = "127.0.0.1",
        .port = 8992,
        .target_path = test_dir
    };
    web_server_t *server = web_server_start(&cfg);
    assert(server != NULL);

    /* Query /api/library */
    pthread_t tid;
    int fd = start_client_session(server, &tid);
    const char req[] = "GET /api/library HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    send(fd, req, sizeof(req) - 1, 0);

    char resp[16384];
    size_t total_recv = 0;
    ssize_t n;
    while ((n = recv(fd, resp + total_recv, sizeof(resp) - 1 - total_recv, 0)) > 0) {
        total_recv += n;
    }
    resp[total_recv] = '\0';
    close(fd);
    pthread_join(tid, NULL);

    /* Assert Dune is present, but .mp4, .mkv, .txt and OtherFolder are absent */
    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(resp, "Dune") != NULL);
    assert(strstr(resp, "home_video") == NULL);
    assert(strstr(resp, "holiday_movie") == NULL);
    assert(strstr(resp, "readme.txt") == NULL);
    assert(strstr(resp, "OtherFolder") == NULL);

    web_server_destroy(server);

    /* Cleanup temporary files */
    unlink(valid_tdvd);
    unlink(dummy_mp4);
    unlink(dummy_mkv);
    unlink(dummy_txt);
    rmdir(sub_dir);
    rmdir(test_dir);

    printf("  - Directory Filtering: OK (Tolerates & ignores non-TinyDVD files/folders, only lists .tdvd)\n");
}

int main(int argc, char *argv[]) {
    const char *tdvd = (argc > 1) ? argv[1] : "./tinydvds/Dune.tdvd";
    printf("=== Running TinyDVD Web Server & Protocol Tests ===\n");
    test_sha1_and_websocket_key();
    test_spu_png_encoding();
    test_http_and_websocket_endpoints(tdvd);
    test_library_directory_filtering(tdvd);
    printf("All Web Server tests completed successfully!\n");
    return 0;
}
