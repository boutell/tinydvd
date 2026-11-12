#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <fcntl.h>
#include "web_server.h"

typedef struct {
    web_server_t *server;
    int fd;
} s_arg_t;

static void *server_thread(void *arg) {
    s_arg_t *a = (s_arg_t *)arg;
    web_server_handle_client(a->server, a->fd);
    free(a);
    return NULL;
}

static void send_ws_text(int fd, const char *msg) {
    size_t mlen = strlen(msg);
    uint8_t frame[512];
    frame[0] = 0x81;
    frame[1] = 0x80 | (uint8_t)mlen;
    uint8_t mask[4] = { 0x12, 0x34, 0x56, 0x78 };
    memcpy(&frame[2], mask, 4);
    for (size_t i = 0; i < mlen; i++) {
        frame[6 + i] = (uint8_t)msg[i] ^ mask[i % 4];
    }
    send(fd, frame, 6 + mlen, 0);
}

int main(void) {
    web_server_config_t cfg = {
        .bind_addr = "127.0.0.1",
        .port = 8993,
        .target_path = "./tinydvds/eulogy.tdvd"
    };
    web_server_t *server = web_server_start(&cfg);
    if (!server) {
        printf("Failed to start server\n");
        return 1;
    }

    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    fcntl(sv[1], F_SETFL, O_NONBLOCK);

    s_arg_t *arg = malloc(sizeof(*arg));
    arg->server = server;
    arg->fd = sv[0];
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, arg);

    const char ws_handshake[] =
        "GET /ws?session=test_eulogy HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    send(sv[1], ws_handshake, strlen(ws_handshake), 0);

    /* Read handshake resp */
    char buf[4096];
    usleep(50000);
    recv(sv[1], buf, sizeof(buf), 0);

    printf("Starting loop simulation...\n");
    /* Read incoming WS messages for 30 seconds while simulating video progress */
    double cur_pts = 0.0;
    bool in_scene = false;
    for (int sec = 0; sec < 100; sec++) {
        usleep(250000); /* 250ms */
        ssize_t n = recv(sv[1], buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            /* Parse simple frames */
            char *p = buf;
            while (p < buf + n) {
                if ((uint8_t)p[0] == 0x81) {
                    size_t plen = (uint8_t)p[1] & 0x7F;
                    char msg[4096] = {0};
                    if (plen < 126 && p + 2 + plen <= buf + n) {
                        memcpy(msg, p + 2, plen);
                        msg[plen] = '\0';
                        if (strstr(msg, "\"highlight\"")) {
                            char *btns = strstr(msg, "\"buttons\":");
                            char *act = strstr(msg, "\"active_btn\":");
                            printf("[%02d] HIGHLIGHT active=%c, btns=%.40s\n", sec, act ? act[13] : '?', btns ? btns + 10 : "");
                            /* If on main menu and scene selection button exists, click it */
                            if (!in_scene && strstr(msg, "\"btn\":2")) {
                                printf("[%02d] Clicking Button 2 (Scene Selection)...\n", sec);
                                in_scene = true;
                                send_ws_text(sv[1], "{\"type\":\"key\",\"key\":\"Down\"}");
                                usleep(50000);
                                send_ws_text(sv[1], "{\"type\":\"key\",\"key\":\"Activate\"}");
                            }
                        } else {
                            printf("[%02d] WS MSG: %s\n", sec, msg);
                        }
                        if (strstr(msg, "\"seek\"")) {
                            char *pt = strstr(msg, "\"pts\":");
                            if (pt) cur_pts = atof(pt + 6);
                        }
                        p += 2 + plen;
                    } else {
                        break;
                    }
                } else {
                    p++;
                }
            }
        }
        /* Advance time update */
        cur_pts += 0.25;
        char tu[64];
        snprintf(tu, sizeof(tu), "{\"type\":\"time_update\",\"pts\":%.3f}", cur_pts);
        send_ws_text(sv[1], tu);
    }

    close(sv[1]);
    return 0;
}
