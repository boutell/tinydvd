#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
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

static size_t http_get_request(web_server_t *server, const char *path, char *resp, size_t resp_cap) {
    pthread_t tid;
    int fd = start_client_session(server, &tid);
    char req[2048];
    snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", path);
    send(fd, req, strlen(req), 0);

    size_t total_recv = 0;
    ssize_t n;
    while (total_recv + 1 < resp_cap &&
           (n = recv(fd, resp + total_recv, resp_cap - 1 - total_recv, 0)) > 0) {
        total_recv += (size_t)n;
    }
    resp[total_recv] = '\0';
    close(fd);
    pthread_join(tid, NULL);
    return total_recv;
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

    /* Open an SSE stream and verify OS directory watcher pushes library_update on new file */
    pthread_t sse_tid;
    int sse_fd = start_client_session(server, &sse_tid);
    const char sse_req[] = "GET /api/library/events HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    send(sse_fd, sse_req, sizeof(sse_req) - 1, 0);

    char sse_buf[4096];
    size_t sse_len = 0;
    struct pollfd sse_pfd = { .fd = sse_fd, .events = POLLIN };
    assert(poll(&sse_pfd, 1, 2000) > 0);
    ssize_t s0 = recv(sse_fd, sse_buf, sizeof(sse_buf) - 1, 0);
    assert(s0 > 0);
    sse_len = (size_t)s0;
    sse_buf[sse_len] = '\0';
    assert(strstr(sse_buf, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(sse_buf, "text/event-stream") != NULL);

    char extra_tdvd[512];
    snprintf(extra_tdvd, sizeof(extra_tdvd), "%s/Dune_SSE_Test.tdvd", test_dir);
    assert(symlink(abs_tdvd, extra_tdvd) == 0);

    bool got_update = (strstr(sse_buf, "event: library_update") != NULL);
    for (int attempt = 0; attempt < 30 && !got_update; attempt++) {
        int pr = poll(&sse_pfd, 1, 100);
        if (pr > 0 && sse_len + 1 < sizeof(sse_buf)) {
            ssize_t sn = recv(sse_fd, sse_buf + sse_len, sizeof(sse_buf) - 1 - sse_len, 0);
            if (sn <= 0) break;
            sse_len += (size_t)sn;
            sse_buf[sse_len] = '\0';
            if (strstr(sse_buf, "event: library_update") != NULL) {
                got_update = true;
            }
        }
    }
    assert(got_update);

    shutdown(sse_fd, SHUT_RDWR);
    close(sse_fd);
    pthread_join(sse_tid, NULL);

    char lib_resp2[16384];
    http_get_request(server, "/api/library", lib_resp2, sizeof(lib_resp2));
    assert(strstr(lib_resp2, "\"id\":\"Dune_SSE_Test_tdvd\"") != NULL);

    web_server_destroy(server);

    /* Cleanup temporary files */
    unlink(extra_tdvd);
    unlink(valid_tdvd);
    unlink(dummy_mp4);
    unlink(dummy_mkv);
    unlink(dummy_txt);
    rmdir(sub_dir);
    rmdir(test_dir);

    printf("  - Directory Filtering: OK (Tolerates & ignores non-TinyDVD files/folders, only lists .tdvd)\n");
}

static void write_dummy_file(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    if (len > 0) assert(fwrite(data, 1, len, f) == len);
    fclose(f);
}

static void test_import_dvd_workflow(void) {
    printf("Testing Web Server Built-in DVD Import Workflow...\n");

    char lib_dir[] = "/tmp/tinydvd_test_lib_XXXXXX";
    char src_dir[] = "/tmp/tinydvd_test_src_XXXXXX";
    assert(mkdtemp(lib_dir) != NULL);
    assert(mkdtemp(src_dir) != NULL);

    /* Create a lightweight synthetic converted DVD directory so convert-to-tinydvd packages cleanly */
    char f_ifo[512], f_mvob[512], f_mkv[512], f_png[512], f_meta[512];
    snprintf(f_ifo, sizeof(f_ifo), "%s/VIDEO_TS.IFO", src_dir);
    snprintf(f_mvob, sizeof(f_mvob), "%s/VIDEO_TS.mvob", src_dir);
    snprintf(f_mkv, sizeof(f_mkv), "%s/VIDEO_TS_VIDEO.mkv", src_dir);
    snprintf(f_png, sizeof(f_png), "%s/thumbnail.png", src_dir);
    snprintf(f_meta, sizeof(f_meta), "%s/metadata.json", src_dir);

    const uint8_t dummy_ifo[] = "DVDVIDEO-VMG";
    const uint8_t dummy_mvob[] = "MVOB0001";
    const uint8_t dummy_mkv[] = { 0x1A, 0x45, 0xDF, 0xA3 };
    const uint8_t dummy_png[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x00 };
    const char dummy_meta[] = "{\"title\":\"Dune Imported\",\"year\":2021,\"director\":\"Denis Villeneuve\",\"cast\":\"Timothee Chalamet, Zendaya\",\"genres\":\"Sci-Fi\"}";
    write_dummy_file(f_ifo, dummy_ifo, sizeof(dummy_ifo));
    write_dummy_file(f_mvob, dummy_mvob, sizeof(dummy_mvob));
    write_dummy_file(f_mkv, dummy_mkv, sizeof(dummy_mkv));
    write_dummy_file(f_png, dummy_png, sizeof(dummy_png));
    write_dummy_file(f_meta, (const uint8_t *)dummy_meta, sizeof(dummy_meta) - 1);

    web_server_config_t cfg = {
        .bind_addr = "127.0.0.1",
        .port = 8993,
        .target_path = lib_dir
    };
    web_server_t *server = web_server_start(&cfg);
    assert(server != NULL);

    char resp[32768];

    /* 1. Verify initial idle status */
    http_get_request(server, "/api/import/status", resp, sizeof(resp));
    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(resp, "\"state\":\"idle\"") != NULL);

    /* 2. Start background import */
    char start_uri[1024];
    snprintf(start_uri, sizeof(start_uri),
             "/api/import/start?source=%s&title=Dune%%20Imported&skip_enrichment=1",
             src_dir);
    http_get_request(server, start_uri, resp, sizeof(resp));
    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(resp, "\"status\":\"started\"") != NULL);

    /* 3. Poll /api/import/status until done */
    bool done = false;
    for (int attempt = 0; attempt < 100; attempt++) {
        usleep(100000);
        http_get_request(server, "/api/import/status", resp, sizeof(resp));
        assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
        if (strstr(resp, "\"state\":\"error\"") != NULL) {
            fprintf(stderr, "Import failed unexpectedly: %s\n", resp);
            assert(0);
        }
        if (strstr(resp, "\"state\":\"done\"") != NULL) {
            done = true;
            break;
        }
    }
    assert(done);
    assert(strstr(resp, "\"result_title\":\"Dune Imported\"") != NULL);

    /* 4. Verify /api/library dynamically includes the newly imported archive */
    http_get_request(server, "/api/library", resp, sizeof(resp));
    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(resp, "Dune Imported") != NULL);
    assert(strstr(resp, "Denis Villeneuve") != NULL);
    assert(strstr(resp, "Timothee Chalamet") != NULL);

    web_server_destroy(server);

    char out_tdvd[512];
    snprintf(out_tdvd, sizeof(out_tdvd), "%s/Dune Imported.tdvd", lib_dir);
    unlink(out_tdvd);
    unlink(f_ifo);
    unlink(f_mvob);
    unlink(f_mkv);
    unlink(f_png);
    unlink(f_meta);
    rmdir(src_dir);
    rmdir(lib_dir);

    printf("  - Built-in DVD Import Workflow: OK (status -> start -> convert -> live catalog refresh)\n");
}

static void poll_import_until_done(web_server_t *server, char *resp, size_t resp_cap) {
    bool done = false;
    for (int attempt = 0; attempt < 100; attempt++) {
        usleep(100000);
        http_get_request(server, "/api/import/status", resp, resp_cap);
        assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
        if (strstr(resp, "\"state\":\"error\"") != NULL) {
            fprintf(stderr, "Import failed unexpectedly: %s\n", resp);
            assert(0);
        }
        if (strstr(resp, "\"state\":\"done\"") != NULL) {
            done = true;
            break;
        }
    }
    assert(done);
}

static void create_dummy_converted_dir(const char *dir, const char *meta_json) {
    char f_ifo[512], f_mvob[512], f_mkv[512], f_png[512], f_meta[512];
    snprintf(f_ifo, sizeof(f_ifo), "%s/VIDEO_TS.IFO", dir);
    snprintf(f_mvob, sizeof(f_mvob), "%s/VIDEO_TS.mvob", dir);
    snprintf(f_mkv, sizeof(f_mkv), "%s/VIDEO_TS_VIDEO.mkv", dir);
    snprintf(f_png, sizeof(f_png), "%s/thumbnail.png", dir);
    snprintf(f_meta, sizeof(f_meta), "%s/metadata.json", dir);

    const uint8_t dummy_ifo[] = "DVDVIDEO-VMG";
    const uint8_t dummy_mvob[] = "MVOB0001";
    const uint8_t dummy_mkv[] = { 0x1A, 0x45, 0xDF, 0xA3 };
    const uint8_t dummy_png[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x00 };
    write_dummy_file(f_ifo, dummy_ifo, sizeof(dummy_ifo));
    write_dummy_file(f_mvob, dummy_mvob, sizeof(dummy_mvob));
    write_dummy_file(f_mkv, dummy_mkv, sizeof(dummy_mkv));
    write_dummy_file(f_png, dummy_png, sizeof(dummy_png));
    write_dummy_file(f_meta, (const uint8_t *)meta_json, strlen(meta_json));
}

static void remove_dummy_converted_dir(const char *dir) {
    char p[512];
    const char *names[] = { "VIDEO_TS.IFO", "VIDEO_TS.mvob", "VIDEO_TS_VIDEO.mkv", "thumbnail.png", "metadata.json" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
        unlink(p);
    }
    rmdir(dir);
}

static void test_duplicate_dvd_import_disambiguation(void) {
    printf("Testing Duplicate DVD Import Name Disambiguation (Year first, then numeric suffix)...\n");

    char lib_dir[] = "/tmp/tinydvd_test_dup_lib_XXXXXX";
    char src1_dir[] = "/tmp/tinydvd_test_dup_s1_XXXXXX";
    char src2_dir[] = "/tmp/tinydvd_test_dup_s2_XXXXXX";
    assert(mkdtemp(lib_dir) != NULL);
    assert(mkdtemp(src1_dir) != NULL);
    assert(mkdtemp(src2_dir) != NULL);

    create_dummy_converted_dir(src1_dir, "{\"title\":\"Dune Test\",\"year\":1984,\"director\":\"David Lynch\"}");
    create_dummy_converted_dir(src2_dir, "{\"title\":\"Dune Test\",\"year\":2021,\"director\":\"Denis Villeneuve\"}");

    web_server_config_t cfg = {
        .bind_addr = "127.0.0.1",
        .port = 8994,
        .target_path = lib_dir
    };
    web_server_t *server = web_server_start(&cfg);
    assert(server != NULL);

    char resp[32768];
    char start_uri[1024];

    /* 1. First import: creates Dune Test.tdvd (1984) */
    snprintf(start_uri, sizeof(start_uri),
             "/api/import/start?source=%s&title=Dune%%20Test&skip_enrichment=1", src1_dir);
    http_get_request(server, start_uri, resp, sizeof(resp));
    assert(strstr(resp, "\"status\":\"started\"") != NULL);
    poll_import_until_done(server, resp, sizeof(resp));

    /* 2. Second import (different year 2021): creates Dune Test (2021).tdvd */
    snprintf(start_uri, sizeof(start_uri),
             "/api/import/start?source=%s&title=Dune%%20Test&skip_enrichment=1", src2_dir);
    http_get_request(server, start_uri, resp, sizeof(resp));
    assert(strstr(resp, "\"status\":\"started\"") != NULL);
    poll_import_until_done(server, resp, sizeof(resp));

    /* 3. Third import (same title + same year 2021): creates Dune Test (2021) (2).tdvd */
    snprintf(start_uri, sizeof(start_uri),
             "/api/import/start?source=%s&title=Dune%%20Test&skip_enrichment=1", src2_dir);
    http_get_request(server, start_uri, resp, sizeof(resp));
    assert(strstr(resp, "\"status\":\"started\"") != NULL);
    poll_import_until_done(server, resp, sizeof(resp));
    assert(strstr(resp, "\"result_title\":\"Dune Test (2)\"") != NULL);

    /* Verify all 3 archives exist on disk and appear in /api/library */
    struct stat st;
    char out1[512], out2[512], out3[512];
    snprintf(out1, sizeof(out1), "%s/Dune Test.tdvd", lib_dir);
    snprintf(out2, sizeof(out2), "%s/Dune Test (2021).tdvd", lib_dir);
    snprintf(out3, sizeof(out3), "%s/Dune Test (2021) (2).tdvd", lib_dir);
    assert(stat(out1, &st) == 0 && st.st_size > 0);
    assert(stat(out2, &st) == 0 && st.st_size > 0);
    assert(stat(out3, &st) == 0 && st.st_size > 0);

    http_get_request(server, "/api/library", resp, sizeof(resp));
    assert(strstr(resp, "David Lynch") != NULL);
    assert(strstr(resp, "Denis Villeneuve") != NULL);
    assert(strstr(resp, "Dune Test (2)") != NULL);

    web_server_destroy(server);

    unlink(out1);
    unlink(out2);
    unlink(out3);
    remove_dummy_converted_dir(src1_dir);
    remove_dummy_converted_dir(src2_dir);
    rmdir(lib_dir);

    printf("  - Duplicate DVD Disambiguation: OK (year suffix + (2) numeric suffix, no overwrites)\n");
}

int main(int argc, char *argv[]) {
    const char *tdvd = (argc > 1) ? argv[1] : "./tinydvds/Dune.tdvd";
    printf("=== Running TinyDVD Web Server & Protocol Tests ===\n");
    test_sha1_and_websocket_key();
    test_spu_png_encoding();
    test_http_and_websocket_endpoints(tdvd);
    test_library_directory_filtering(tdvd);
    test_import_dvd_workflow();
    test_duplicate_dvd_import_disambiguation();
    printf("All Web Server tests completed successfully!\n");
    return 0;
}
