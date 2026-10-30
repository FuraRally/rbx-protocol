/**
 * @file rbx_pilot_client.c
 * @brief Pilot-screen client: subscribe to the main Rabbit 2 phone and keep a decoded state.
 *
 * Behaviour (as the official pilot, plus re-subscription): send 0x01 to <main>:7532 every hello_ms
 * until data arrives; if nothing arrives for stale_ms, report the link down and subscribe again.
 * Datagrams from other IPs are ignored.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "rbx_net.h"
#include "rbx_pilot_client.h"

static const char *TAG = "rbx_pilot_cli";

#define POLL_MS 100

struct rbx_pilot_client {
    rbx_pilot_client_config_t cfg;
    volatile uint32_t target;
    int sock;
    rbx_splitter_t splitter;
    rbx_pilot_state_t state;
    uint16_t changed;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t done;
    TaskHandle_t task;
    volatile bool running;
    volatile bool receiving;
};

static void on_record(const char *rec, size_t len, void *ctx)
{
    struct rbx_pilot_client *c = ctx;
    c->changed |= rbx_pilot_apply(&c->state, rec, len);
}

static void set_receiving(struct rbx_pilot_client *c, bool receiving)
{
    if (c->receiving == receiving)
        return;
    c->receiving = receiving;
    char ip[16];
    rbx_peer_t p = { .addr = c->target };
    ESP_LOGI(TAG, "%s %s", rbx_peer_ip_str(&p, ip, sizeof(ip)), receiving ? "streaming" : "silent");
    if (c->cfg.on_link)
        c->cfg.on_link(receiving, c->cfg.ctx);
}

static void send_hello(struct rbx_pilot_client *c, uint32_t target)
{
    const char hello = 0x01;
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = htons(RBX_PILOT_PORT),
        .sin_addr.s_addr = target,
    };
    sendto(c->sock, &hello, 1, 0, (struct sockaddr *)&to, sizeof(to));
}

static void client_task(void *arg)
{
    struct rbx_pilot_client *c = arg;
    char buf[512];
    int64_t last_rx = 0, last_hello = 0;
    uint32_t current = 0;

    while (c->running) {
        uint32_t target = c->target;
        int64_t now = rbx_now_ms();
        if (target != current) {
            /* New main phone: start over. */
            current = target;
            last_rx = 0;
            last_hello = 0;
            set_receiving(c, false);
            xSemaphoreTake(c->lock, portMAX_DELAY);
            rbx_splitter_init(&c->splitter);
            rbx_pilot_state_init(&c->state);
            xSemaphoreGive(c->lock);
        }
        if (last_rx && now - last_rx > (int64_t)c->cfg.stale_ms) {
            set_receiving(c, false);
            last_rx = 0;
        }
        if (target && !last_rx && now - last_hello >= c->cfg.hello_ms) {
            send_hello(c, target);
            last_hello = now;
        }

        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(c->sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (n <= 0 || !target || from.sin_addr.s_addr != target)
            continue;

        last_rx = rbx_now_ms();
        set_receiving(c, true);
        xSemaphoreTake(c->lock, portMAX_DELAY);
        c->changed = 0;
        rbx_splitter_feed(&c->splitter, buf, n, on_record, c);
        uint16_t changed = c->changed;
        xSemaphoreGive(c->lock);
        if (changed && c->cfg.on_update)
            c->cfg.on_update(&c->state, changed, c->cfg.ctx);
    }
    close(c->sock);
    c->sock = -1;
    xSemaphoreGive(c->done);
    vTaskDelete(NULL);
}

esp_err_t rbx_pilot_client_start(const rbx_pilot_client_config_t *config, rbx_pilot_client_handle_t *out)
{
    if (!config || !out)
        return ESP_ERR_INVALID_ARG;
    struct rbx_pilot_client *c = calloc(1, sizeof(*c));
    if (!c)
        return ESP_ERR_NO_MEM;
    c->cfg = *config;
    if (!c->cfg.hello_ms)
        c->cfg.hello_ms = 500;
    if (!c->cfg.stale_ms)
        c->cfg.stale_ms = 3000;
    c->target = c->cfg.main_addr;
    rbx_pilot_state_init(&c->state);
    rbx_splitter_init(&c->splitter);

    c->sock = rbx_udp_socket(c->cfg.local_port);
    if (c->sock < 0) {
        free(c);
        return ESP_FAIL;
    }
    rbx_set_timeouts(c->sock, POLL_MS, 0);
    c->lock = xSemaphoreCreateMutex();
    c->done = xSemaphoreCreateBinary();
    c->running = true;
    if (!c->lock || !c->done ||
        xTaskCreate(client_task, "rbx_pilot_cli", CONFIG_RBX_TASK_STACK, c, CONFIG_RBX_TASK_PRIORITY, &c->task) !=
            pdPASS) {
        close(c->sock);
        if (c->lock)
            vSemaphoreDelete(c->lock);
        if (c->done)
            vSemaphoreDelete(c->done);
        free(c);
        return ESP_ERR_NO_MEM;
    }
    *out = c;
    return ESP_OK;
}

void rbx_pilot_client_stop(rbx_pilot_client_handle_t c)
{
    if (!c)
        return;
    c->running = false;
    if (xSemaphoreTake(c->done, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "Client task did not stop; leaking it");
        return;
    }
    vSemaphoreDelete(c->lock);
    vSemaphoreDelete(c->done);
    free(c);
}

void rbx_pilot_client_set_target(rbx_pilot_client_handle_t c, uint32_t main_addr)
{
    if (c)
        c->target = main_addr;
}

uint32_t rbx_pilot_client_target(rbx_pilot_client_handle_t c)
{
    return c ? c->target : 0;
}

void rbx_pilot_client_get_state(rbx_pilot_client_handle_t c, rbx_pilot_state_t *out)
{
    xSemaphoreTake(c->lock, portMAX_DELAY);
    *out = c->state;
    xSemaphoreGive(c->lock);
}

bool rbx_pilot_client_receiving(rbx_pilot_client_handle_t c)
{
    return c && c->receiving;
}
