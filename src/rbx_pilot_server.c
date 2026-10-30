/**
 * @file rbx_pilot_server.c
 * @brief Pilot server with the main app's rules (see rbx_pilot_server.h).
 */
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "rbx_net.h"
#include "rbx_pilot_server.h"

static const char *TAG = "rbx_pilot_srv";

#define FULL_RESEND_DELAY_MS 2000
#define TAG6_REPEAT_DELAY_MS 2000

static const char *const PAGE_ORDER[] = {
    [RBX_PILOT_PAGE_NORMAL] = "156720",
    [RBX_PILOT_PAGE_TABLAS] = "146720",
    [RBX_PILOT_PAGE_DOUBLE_TRIP] = "890",
};

static struct {
    rbx_pilot_server_config_t cfg;
    int sock;
    struct sockaddr_in pilots[CONFIG_RBX_MAX_PILOTS];
    int pilot_count;
    char current[10][RBX_PILOT_TEXT_MAX + 2];
    char sent[10][RBX_PILOT_TEXT_MAX + 2];
    rbx_pilot_page_t page;
    int64_t full_resend_at; /* 0 = none */
    int64_t tag6_repeat_at;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t done;
    volatile bool running;
} s = { .sock = -1 };

static void register_pilot(const struct sockaddr_in *from)
{
    bool known = false;
    for (int i = 0; i < s.pilot_count; i++)
        if (s.pilots[i].sin_addr.s_addr == from->sin_addr.s_addr)
            known = true;
    if (!known) {
        /* Like the app: registered by IP; answers go to 7532 unless reply_to_source (tests). */
        int slot = (s.pilot_count < s.cfg.max_pilots) ? s.pilot_count++ : 0;
        s.pilots[slot] = *from;
        if (!s.cfg.reply_to_source)
            s.pilots[slot].sin_port = htons(RBX_PILOT_PORT);
        rbx_peer_t peer = { .addr = from->sin_addr.s_addr,
                            .port = ntohs(s.pilots[slot].sin_port),
                            .transport = RBX_TRANSPORT_UDP };
        char ip[16];
        ESP_LOGI(TAG, "Pilot %s registered", rbx_peer_ip_str(&peer, ip, sizeof(ip)));
        if (s.cfg.on_pilot)
            s.cfg.on_pilot(&peer, s.cfg.ctx);
    }
    /* Any datagram schedules a full resend to everyone 2 s later. */
    s.full_resend_at = rbx_now_ms() + FULL_RESEND_DELAY_MS;
}

static void send_dirty(void)
{
    char out[512];
    size_t len = 0;
    int64_t now = rbx_now_ms();

    if (s.full_resend_at && now >= s.full_resend_at) {
        memset(s.sent, 0, sizeof(s.sent));
        s.full_resend_at = 0;
    }
    if (s.tag6_repeat_at && now >= s.tag6_repeat_at) {
        s.sent[6][0] = '\0';
        s.tag6_repeat_at = 0;
    }
    for (const char *t = PAGE_ORDER[s.page]; *t; t++) {
        int i = *t - '0';
        const char *rec = s.current[i];
        if (!rec[0] || strcmp(rec, s.sent[i]) == 0)
            continue;
        size_t n = strlen(rec);
        if (len + n >= sizeof(out))
            break;
        memcpy(out + len, rec, n);
        len += n;
        if (i == 6 && s.sent[6][0])
            s.tag6_repeat_at = now + TAG6_REPEAT_DELAY_MS; /* the app repeats tag 6 once after a change */
        strcpy(s.sent[i], rec);
    }
    if (!len)
        return;
    for (int p = 0; p < s.pilot_count; p++)
        sendto(s.sock, out, len, 0, (struct sockaddr *)&s.pilots[p], sizeof(s.pilots[p]));
}

static void server_task(void *arg)
{
    (void)arg;
    int64_t next = rbx_now_ms();
    char buf[64];
    while (s.running) {
        int64_t wait = next - rbx_now_ms();
        if (wait < 0)
            wait = 0;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s.sock, &rfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = (long)wait * 1000 };
        if (select(s.sock + 1, &rfds, NULL, NULL, &tv) > 0) {
            struct sockaddr_in from;
            socklen_t flen = sizeof(from);
            if (recvfrom(s.sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen) >= 0) {
                xSemaphoreTake(s.lock, portMAX_DELAY);
                register_pilot(&from);
                xSemaphoreGive(s.lock);
            }
        }
        if (rbx_now_ms() >= next) {
            next += s.cfg.period_ms;
            if (next < rbx_now_ms())
                next = rbx_now_ms() + s.cfg.period_ms;
            xSemaphoreTake(s.lock, portMAX_DELAY);
            if (s.pilot_count)
                send_dirty();
            xSemaphoreGive(s.lock);
        }
    }
    close(s.sock);
    s.sock = -1;
    xSemaphoreGive(s.done);
    vTaskDelete(NULL);
}

esp_err_t rbx_pilot_server_start(const rbx_pilot_server_config_t *config)
{
    if (s.running)
        return ESP_ERR_INVALID_STATE;
    if (!config)
        return ESP_ERR_INVALID_ARG;
    s.cfg = *config;
    if (!s.cfg.period_ms)
        s.cfg.period_ms = 30;
    if (s.cfg.max_pilots == 0 || s.cfg.max_pilots > CONFIG_RBX_MAX_PILOTS)
        s.cfg.max_pilots = CONFIG_RBX_MAX_PILOTS;
    s.pilot_count = 0;
    s.full_resend_at = s.tag6_repeat_at = 0;
    memset(s.sent, 0, sizeof(s.sent));
    if (!s.lock)
        s.lock = xSemaphoreCreateMutex();
    if (!s.done)
        s.done = xSemaphoreCreateBinary();
    if (!s.lock || !s.done)
        return ESP_ERR_NO_MEM;
    if ((s.sock = rbx_udp_socket(s.cfg.port)) < 0)
        return ESP_FAIL;
    s.running = true;
    if (xTaskCreate(server_task, "rbx_pilot_srv", CONFIG_RBX_TASK_STACK, NULL, CONFIG_RBX_TASK_PRIORITY, NULL) !=
        pdPASS) {
        s.running = false;
        close(s.sock);
        s.sock = -1;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Serving pilots on UDP %u, period %u ms", s.cfg.port, s.cfg.period_ms);
    return ESP_OK;
}

void rbx_pilot_server_stop(void)
{
    if (!s.running)
        return;
    s.running = false;
    xSemaphoreTake(s.done, pdMS_TO_TICKS(2000));
}

int rbx_pilot_server_pilot_count(void)
{
    return s.pilot_count;
}

esp_err_t rbx_pilot_server_set(char tag, const char *record)
{
    if (tag < '0' || tag > '9' || tag == '3')
        return ESP_ERR_INVALID_ARG;
    if (record && (record[0] != tag || strlen(record) >= sizeof(s.current[0])))
        return ESP_ERR_INVALID_ARG;
    if (!s.lock)
        s.lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s.lock, portMAX_DELAY);
    if (record)
        strcpy(s.current[tag - '0'], record);
    else
        s.current[tag - '0'][0] = '\0';
    xSemaphoreGive(s.lock);
    return ESP_OK;
}

void rbx_pilot_server_set_page(rbx_pilot_page_t page)
{
    if (page <= RBX_PILOT_PAGE_DOUBLE_TRIP)
        s.page = page;
}
