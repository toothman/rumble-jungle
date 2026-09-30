// SPDX-License-Identifier: MIT
/*
 * Minimal HTTP/1.0 streaming server. Every client receives the same live
 * stream of ADTS-AAC frames. Slow or disconnected clients are dropped so
 * they cannot stall the others.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "app_config.h"
#include "audio.h"
#include "stream_server.h"

#define RECV_TIMEOUT_S   2
#define SEND_TIMEOUT_US  500000

static const char *TAG = "stream";
static int s_clients[STREAM_MAX_CLIENTS];
static SemaphoreHandle_t s_lock;

static const char HDR_OK[] =
    "HTTP/1.0 200 OK\r\n"
    "Content-Type: audio/aac\r\n"
    "Cache-Control: no-cache, no-store\r\n"
    "Connection: close\r\n"
    "icy-name: " STREAM_NAME "\r\n\r\n";
static const char HDR_404[]  = "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n";
static const char HDR_BUSY[] = "HTTP/1.0 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n";

int stream_server_client_count(void)
{
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < STREAM_MAX_CLIENTS; i++) {
        if (s_clients[i] >= 0) n++;
    }
    xSemaphoreGive(s_lock);
    return n;
}

static bool send_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len > 0) {
        int n = send(fd, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

static void handle_new_client(int fd)
{
    struct timeval tv = { .tv_sec = RECV_TIMEOUT_S };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char req[512];
    int n = recv(fd, req, sizeof(req) - 1, 0);
    if (n <= 0) { close(fd); return; }
    req[n] = '\0';

    /* Accept "GET /stream.aac" and "HEAD /stream.aac" (query string allowed) */
    bool is_get  = strncmp(req, "GET ", 4) == 0;
    bool is_head = strncmp(req, "HEAD ", 5) == 0;
    const char *path = req + (is_get ? 4 : 5);
    size_t plen = strlen(STREAM_PATH);
    bool match = (is_get || is_head) && strncmp(path, STREAM_PATH, plen) == 0 &&
                 (path[plen] == ' ' || path[plen] == '?');

    tv = (struct timeval){ .tv_sec = 0, .tv_usec = SEND_TIMEOUT_US };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (!match)  { send_all(fd, HDR_404, strlen(HDR_404)); close(fd); return; }
    if (is_head) { send_all(fd, HDR_OK,  strlen(HDR_OK));  close(fd); return; }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < STREAM_MAX_CLIENTS; i++) {
        if (s_clients[i] < 0) { slot = i; break; }
    }
    if (slot < 0 || !send_all(fd, HDR_OK, strlen(HDR_OK))) {
        xSemaphoreGive(s_lock);
        if (slot < 0) send_all(fd, HDR_BUSY, strlen(HDR_BUSY));
        close(fd);
        return;
    }
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    s_clients[slot] = fd;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Client connected (slot %d)", slot);
}

static void accept_task(void *arg)
{
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(STREAM_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(ls, 4) != 0) {
        ESP_LOGE(TAG, "Cannot listen on port %d", STREAM_PORT);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "Stream server listening on port %d", STREAM_PORT);

    while (true) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        handle_new_client(fd);
    }
}

static void sender_task(void *arg)
{
    RingbufHandle_t rb = audio_get_output();
    while (true) {
        size_t len = 0;
        uint8_t *frame = xRingbufferReceive(rb, &len, portMAX_DELAY);
        if (!frame) continue;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < STREAM_MAX_CLIENTS; i++) {
            if (s_clients[i] >= 0 && !send_all(s_clients[i], frame, len)) {
                close(s_clients[i]);
                s_clients[i] = -1;
                ESP_LOGI(TAG, "Client disconnected (slot %d)", i);
            }
        }
        xSemaphoreGive(s_lock);

        /* Without listeners the frame is simply discarded */
        vRingbufferReturnItem(rb, frame);
    }
}

void stream_server_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < STREAM_MAX_CLIENTS; i++) s_clients[i] = -1;
    xTaskCreatePinnedToCore(accept_task, "stream_acc", 4096, NULL,
                            NET_TASK_PRIORITY - 1, NULL, NET_TASK_CORE);
    xTaskCreatePinnedToCore(sender_task, "stream_tx", 4096, NULL,
                            NET_TASK_PRIORITY, NULL, NET_TASK_CORE);
}
