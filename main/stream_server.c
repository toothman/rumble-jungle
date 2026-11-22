// SPDX-License-Identifier: MIT
/*
 * Minimal HTTP/1.0 streaming server. Every listener receives the same live
 * stream of ADTS-AAC frames through its own FIFO and sender task, so a slow
 * client cannot stall the others. A client that falls more than
 * STREAM_CLIENT_FIFO bytes behind is disconnected.
 *
 * New listeners first get the last STREAM_PREBUFFER_BYTES of audio, which
 * lets players fill their buffer and start almost immediately.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "app_config.h"
#include "settings.h"
#include "stream_server.h"

#define RECV_TIMEOUT_S   2
#define SEND_TIMEOUT_S   5
#define HIST_MAX_FRAMES  256

typedef struct {
    int             fd;            /* -1 = free slot */
    RingbufHandle_t fifo;
    volatile bool   kill;
} client_t;

static const char *TAG = "stream";
static client_t s_clients[STREAM_MAX_CLIENTS];
static SemaphoreHandle_t s_lock;

/* Recent frames for the prebuffer (circular, frame aligned) */
static uint8_t *s_hist;
static size_t s_hist_head, s_hist_used;
static uint16_t s_hist_len[HIST_MAX_FRAMES];
static int s_hist_first, s_hist_count;

static const char HDR_404[]  = "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n";
static const char HDR_BUSY[] = "HTTP/1.0 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n";

int stream_server_client_count(void)
{
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < STREAM_MAX_CLIENTS; i++) {
        if (s_clients[i].fd >= 0 && !s_clients[i].kill) n++;
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

/* ------------------------------------------------------------------------ */
/* Prebuffer (called with s_lock held)                                      */
/* ------------------------------------------------------------------------ */

static void hist_add(const uint8_t *frame, size_t len)
{
    if (len > STREAM_PREBUFFER_BYTES) return;
    while (s_hist_count && (s_hist_used + len > STREAM_PREBUFFER_BYTES ||
                            s_hist_count == HIST_MAX_FRAMES)) {
        s_hist_used -= s_hist_len[s_hist_first];
        s_hist_first = (s_hist_first + 1) % HIST_MAX_FRAMES;
        s_hist_count--;
    }
    const size_t first = len < STREAM_PREBUFFER_BYTES - s_hist_head ? len
                                                                    : STREAM_PREBUFFER_BYTES - s_hist_head;
    memcpy(s_hist + s_hist_head, frame, first);
    memcpy(s_hist, frame + first, len - first);
    s_hist_head = (s_hist_head + len) % STREAM_PREBUFFER_BYTES;
    s_hist_len[(s_hist_first + s_hist_count) % HIST_MAX_FRAMES] = len;
    s_hist_count++;
    s_hist_used += len;
}

static void hist_copy_to(RingbufHandle_t fifo)
{
    size_t start = (s_hist_head + STREAM_PREBUFFER_BYTES - s_hist_used) % STREAM_PREBUFFER_BYTES;
    size_t first = s_hist_used < STREAM_PREBUFFER_BYTES - start ? s_hist_used
                                                                : STREAM_PREBUFFER_BYTES - start;
    if (first) xRingbufferSend(fifo, s_hist + start, first, 0);
    if (s_hist_used > first) xRingbufferSend(fifo, s_hist, s_hist_used - first, 0);
}

/* ------------------------------------------------------------------------ */
/* Clients                                                                  */
/* ------------------------------------------------------------------------ */

static void client_task(void *arg)
{
    client_t *c = arg;
    const int slot = c - s_clients;
    ESP_LOGI(TAG, "Listener connected (slot %d)", slot);

    while (!c->kill) {
        size_t len = 0;
        uint8_t *data = xRingbufferReceiveUpTo(c->fifo, &len, pdMS_TO_TICKS(1000), 4096);
        if (!data) continue;
        bool ok = send_all(c->fd, data, len);
        vRingbufferReturnItem(c->fifo, data);
        if (!ok) c->kill = true;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    RingbufHandle_t fifo = c->fifo;
    close(c->fd);
    c->fd = -1;
    c->fifo = NULL;
    xSemaphoreGive(s_lock);
    vRingbufferDeleteWithCaps(fifo);
    ESP_LOGI(TAG, "Listener disconnected (slot %d)", slot);
    vTaskDelete(NULL);
}

static void build_ok_header(char *buf, size_t len)
{
    settings_t s;
    settings_get(&s);
    snprintf(buf, len,
             "HTTP/1.0 200 OK\r\n"
             "Content-Type: audio/aac\r\n"
             "Cache-Control: no-cache, no-store\r\n"
             "Connection: close\r\n"
             "icy-name: %s\r\n"
             "icy-br: %u\r\n\r\n",
             s.stream.name, (unsigned)(settings_bitrate_bps(s.stream.bitrate) / 1000));
}

static void redirect_to_ui(int fd)
{
    struct sockaddr_in local;
    socklen_t alen = sizeof(local);
    char hdr[160], ip[16] = "";
    if (getsockname(fd, (struct sockaddr *)&local, &alen) == 0) {
        inet_ntoa_r(local.sin_addr, ip, sizeof(ip));
    }
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.0 302 Found\r\nLocation: http://%s/\r\nContent-Length: 0\r\n\r\n", ip);
    send_all(fd, hdr, strlen(hdr));
}

static void handle_new_client(int fd)
{
    struct timeval tv = { .tv_sec = RECV_TIMEOUT_S };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv = (struct timeval){ .tv_sec = SEND_TIMEOUT_S };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char req[512];
    int n = recv(fd, req, sizeof(req) - 1, 0);
    if (n <= 0) { close(fd); return; }
    req[n] = '\0';

    /* Accept "GET /stream.aac" and "HEAD /stream.aac" (query string allowed) */
    const bool is_get  = strncmp(req, "GET ", 4) == 0;
    const bool is_head = strncmp(req, "HEAD ", 5) == 0;
    const char *path = req + (is_get ? 4 : 5);
    const size_t plen = strlen(STREAM_PATH);
    const bool match = (is_get || is_head) && strncmp(path, STREAM_PATH, plen) == 0 &&
                       (path[plen] == ' ' || path[plen] == '?');

    if (!match) {
        if (is_get && strncmp(path, "/ ", 2) == 0) redirect_to_ui(fd);
        else send_all(fd, HDR_404, strlen(HDR_404));
        close(fd);
        return;
    }

    char hdr[256];
    build_ok_header(hdr, sizeof(hdr));
    if (is_head) { send_all(fd, hdr, strlen(hdr)); close(fd); return; }

    /* Find a free slot */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    client_t *c = NULL;
    for (int i = 0; i < STREAM_MAX_CLIENTS; i++) {
        if (s_clients[i].fd < 0) { c = &s_clients[i]; break; }
    }
    if (c) c->fd = fd;   /* reserve */
    xSemaphoreGive(s_lock);

    if (!c) {
        send_all(fd, HDR_BUSY, strlen(HDR_BUSY));
        close(fd);
        return;
    }

    RingbufHandle_t fifo = xRingbufferCreateWithCaps(STREAM_CLIENT_FIFO, RINGBUF_TYPE_BYTEBUF,
                                                     MALLOC_CAP_SPIRAM);
    if (!fifo || !send_all(fd, hdr, strlen(hdr))) {
        if (fifo) vRingbufferDeleteWithCaps(fifo);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        c->fd = -1;
        xSemaphoreGive(s_lock);
        close(fd);
        return;
    }
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    hist_copy_to(fifo);
    c->fifo = fifo;
    c->kill = false;
    xSemaphoreGive(s_lock);

    char name[24];
    snprintf(name, sizeof(name), "stream_tx%d", (int)(c - s_clients));
    if (xTaskCreatePinnedToCore(client_task, name, 3072, c, NET_TASK_PRIORITY, NULL,
                                NET_TASK_CORE) != pdPASS) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        c->fd = -1;
        c->fifo = NULL;
        xSemaphoreGive(s_lock);
        vRingbufferDeleteWithCaps(fifo);
        close(fd);
    }
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

void stream_server_broadcast(const uint8_t *frame, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    hist_add(frame, len);
    for (int i = 0; i < STREAM_MAX_CLIENTS; i++) {
        client_t *c = &s_clients[i];
        if (c->fd < 0 || !c->fifo || c->kill) continue;
        if (xRingbufferSend(c->fifo, frame, len, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Listener %d too slow, disconnecting", i);
            c->kill = true;
        }
    }
    xSemaphoreGive(s_lock);
}

void stream_server_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_hist = heap_caps_malloc(STREAM_PREBUFFER_BYTES, MALLOC_CAP_SPIRAM);
    assert(s_lock && s_hist);
    for (int i = 0; i < STREAM_MAX_CLIENTS; i++) s_clients[i].fd = -1;
    xTaskCreatePinnedToCore(accept_task, "stream_acc", 4096, NULL,
                            NET_TASK_PRIORITY - 1, NULL, NET_TASK_CORE);
}
