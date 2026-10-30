/**
 * @file rbx_box_client.c
 * @brief Client towards a RabbitBox / Rabbit Connect, behaving like Rabbit 2.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "rbx_box_client.h"
#include "rbx_net.h"

static const char *TAG = "rbx_box_cli";

#define CONNECT_TIMEOUT_MS 5000
#define POLL_MS            500
#define HELLO_TRIES        5
#define HELLO_INTERVAL_MS  1000

struct rbx_box_client {
    rbx_box_client_config_t cfg;
    struct sockaddr_in server;
    rbx_splitter_t splitter;
    TaskHandle_t task;
    SemaphoreHandle_t done;
    volatile bool running;
    volatile bool connected;
};

static void on_record(const char *rec, size_t len, void *ctx)
{
    struct rbx_box_client *c = ctx;
    rbx_box_msg_t msg;
    if (!rbx_parse_box_record(rec, len, &msg)) {
        ESP_LOGW(TAG, "Dropping invalid record: %.*s", (int)len, rec);
        return;
    }
    if (c->cfg.on_msg)
        c->cfg.on_msg(&msg, c->cfg.ctx);
}

static void set_connected(struct rbx_box_client *c, bool connected)
{
    if (c->connected == connected)
        return;
    c->connected = connected;
    ESP_LOGI(TAG, "%s %s", c->cfg.host, connected ? "connected" : "disconnected");
    if (c->cfg.on_state)
        c->cfg.on_state(connected, c->cfg.ctx);
}

/** Non-blocking connect with timeout; returns a connected socket or -1. */
static int tcp_connect(struct rbx_box_client *c)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0)
        return -1;
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    int r = connect(sock, (struct sockaddr *)&c->server, sizeof(c->server));
    if (r != 0 && errno != EINPROGRESS) {
        close(sock);
        return -1;
    }
    if (r != 0) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval tv = { .tv_sec = CONNECT_TIMEOUT_MS / 1000, .tv_usec = 0 };
        int err = 0;
        socklen_t len = sizeof(err);
        if (select(sock + 1, NULL, &wfds, NULL, &tv) <= 0 ||
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            close(sock);
            return -1;
        }
    }
    fcntl(sock, F_SETFL, flags);
    rbx_set_timeouts(sock, POLL_MS, 1000);
    return sock;
}

static void run_tcp(struct rbx_box_client *c)
{
    int sock = tcp_connect(c);
    if (sock < 0) {
        ESP_LOGD(TAG, "TCP connect to %s:%u failed", c->cfg.host, c->cfg.port);
        return;
    }
    set_connected(c, true);
    char buf[512];
    const char ack = 0x01;
    while (c->running) {
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n > 0) {
            rbx_splitter_feed(&c->splitter, buf, n, on_record, c);
            if (c->cfg.ack)
                send(sock, &ack, 1, 0);
        }
        else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            break;
        }
    }
    close(sock);
}

static void run_udp(struct rbx_box_client *c)
{
    int sock = rbx_udp_socket(c->cfg.local_port);
    if (sock < 0)
        return;
    rbx_set_timeouts(sock, POLL_MS, 0);
    const char ack = 0x01;
    char buf[512];
    int64_t last_rx = 0, last_hello = 0;
    int hellos = 0;

    while (c->running) {
        int64_t now = rbx_now_ms();
        bool silent = !last_rx || now - last_rx > (int64_t)c->cfg.silence_ms;
        if (silent) {
            if (last_rx) {
                set_connected(c, false);
                last_rx = 0;
                hellos = 0;
            }
            if (hellos >= HELLO_TRIES)
                break; /* give up; the outer loop waits reconnect_ms and retries */
            if (now - last_hello >= HELLO_INTERVAL_MS) {
                sendto(sock, &ack, 1, 0, (struct sockaddr *)&c->server, sizeof(c->server));
                last_hello = now;
                hellos++;
            }
        }
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (n <= 0 || from.sin_addr.s_addr != c->server.sin_addr.s_addr)
            continue;
        last_rx = rbx_now_ms();
        set_connected(c, true);
        rbx_splitter_feed(&c->splitter, buf, n, on_record, c);
        if (c->cfg.ack)
            sendto(sock, &ack, 1, 0, (struct sockaddr *)&from, flen);
    }
    close(sock);
}

static void client_task(void *arg)
{
    struct rbx_box_client *c = arg;
    while (c->running) {
        rbx_splitter_init(&c->splitter);
        if (c->cfg.transport == RBX_TRANSPORT_UDP)
            run_udp(c);
        else
            run_tcp(c);
        set_connected(c, false);
        for (uint32_t waited = 0; c->running && waited < c->cfg.reconnect_ms; waited += 100)
            vTaskDelay(pdMS_TO_TICKS(100));
    }
    xSemaphoreGive(c->done);
    vTaskDelete(NULL);
}

esp_err_t rbx_box_client_start(const rbx_box_client_config_t *config, rbx_box_client_handle_t *out)
{
    if (!config || !out)
        return ESP_ERR_INVALID_ARG;
    struct rbx_box_client *c = calloc(1, sizeof(*c));
    if (!c)
        return ESP_ERR_NO_MEM;
    c->cfg = *config;
    c->cfg.host[sizeof(c->cfg.host) - 1] = '\0';
    c->server.sin_family = AF_INET;
    c->server.sin_port = htons(c->cfg.port);
    if (inet_pton(AF_INET, c->cfg.host, &c->server.sin_addr) != 1) {
        free(c);
        return ESP_ERR_INVALID_ARG;
    }
    c->done = xSemaphoreCreateBinary();
    if (!c->done) {
        free(c);
        return ESP_ERR_NO_MEM;
    }
    c->running = true;
    if (xTaskCreate(client_task, "rbx_box_cli", CONFIG_RBX_TASK_STACK, c, CONFIG_RBX_TASK_PRIORITY, &c->task) !=
        pdPASS) {
        vSemaphoreDelete(c->done);
        free(c);
        return ESP_ERR_NO_MEM;
    }
    *out = c;
    return ESP_OK;
}

void rbx_box_client_stop(rbx_box_client_handle_t c)
{
    if (!c)
        return;
    c->running = false;
    /* Worst case: a TCP connect in progress (CONNECT_TIMEOUT_MS) */
    if (xSemaphoreTake(c->done, pdMS_TO_TICKS(CONNECT_TIMEOUT_MS + 1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Client task did not stop; leaking it");
        return;
    }
    vSemaphoreDelete(c->done);
    free(c);
}

bool rbx_box_client_connected(rbx_box_client_handle_t c)
{
    return c && c->connected;
}
