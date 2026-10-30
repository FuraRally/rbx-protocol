/**
 * @file rbx_box_server.c
 * @brief RabbitBox / Rabbit Connect server: TCP and/or UDP on port 11912.
 *
 * One task owns every socket: it accepts TCP clients, registers UDP clients (any datagram), counts the
 * 0x01 acks and closes clients marked dead by a failed send. Senders only write under the lock.
 */
#include <string.h>
#include <sys/param.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "rbx_box_server.h"
#include "rbx_net.h"

static const char *TAG = "rbx_box_srv";

#define LISTEN_BACKLOG 4

typedef struct {
    int sock;
    bool dead;
    rbx_peer_t peer;
} tcp_client_t;

typedef struct {
    bool used;
    struct sockaddr_in addr;
} udp_client_t;

static struct {
    rbx_box_server_config_t cfg;
    int listen_sock;
    int udp_sock;
    tcp_client_t tcp[CONFIG_RBX_MAX_BOX_CLIENTS];
    udp_client_t udp[CONFIG_RBX_MAX_BOX_CLIENTS];
    SemaphoreHandle_t lock;
    SemaphoreHandle_t done;
    TaskHandle_t task;
    volatile bool running;
    volatile uint32_t acks;
} s = { .listen_sock = -1, .udp_sock = -1 };

static void notify(const rbx_peer_t *peer, bool connected)
{
    char ip[16];
    ESP_LOGI(TAG, "%s client %s:%u %s", peer->transport == RBX_TRANSPORT_TCP ? "TCP" : "UDP",
             rbx_peer_ip_str(peer, ip, sizeof(ip)), peer->port, connected ? "connected" : "disconnected");
    if (s.cfg.on_client)
        s.cfg.on_client(peer, connected, s.cfg.ctx);
}

static int open_listener(uint16_t port)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0)
        return -1;
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(sock, LISTEN_BACKLOG) != 0) {
        ESP_LOGE(TAG, "Unable to listen on TCP %u: errno %d", port, errno);
        close(sock);
        return -1;
    }
    return sock;
}

static void accept_client(void)
{
    struct sockaddr_in from;
    socklen_t len = sizeof(from);
    int sock = accept(s.listen_sock, (struct sockaddr *)&from, &len);
    if (sock < 0)
        return;

    int keep = 1, idle = 30, intvl = 5, cnt = 3, nodelay = 1;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keep, sizeof(keep));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    rbx_set_timeouts(sock, 0, s.cfg.send_timeout_ms);

    rbx_peer_t peer = { .addr = from.sin_addr.s_addr, .port = ntohs(from.sin_port), .transport = RBX_TRANSPORT_TCP };
    bool added = false;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    for (int i = 0; i < s.cfg.max_clients; i++) {
        if (s.tcp[i].sock < 0) {
            s.tcp[i] = (tcp_client_t){ .sock = sock, .dead = false, .peer = peer };
            added = true;
            break;
        }
    }
    xSemaphoreGive(s.lock);
    if (!added) {
        ESP_LOGW(TAG, "Max TCP clients reached, rejecting new client");
        close(sock);
        return;
    }
    notify(&peer, true);
}

static void close_tcp(int i)
{
    rbx_peer_t peer = s.tcp[i].peer;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    shutdown(s.tcp[i].sock, SHUT_RDWR);
    close(s.tcp[i].sock);
    s.tcp[i].sock = -1;
    s.tcp[i].dead = false;
    xSemaphoreGive(s.lock);
    notify(&peer, false);
}

static void count_acks(const char *buf, int len)
{
    for (int i = 0; i < len; i++)
        if (buf[i] == 0x01)
            s.acks++;
}

static void read_udp(void)
{
    char buf[64];
    struct sockaddr_in from;
    socklen_t len = sizeof(from);
    int n = recvfrom(s.udp_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &len);
    if (n <= 0)
        return;
    count_acks(buf, n);

    int free_slot = -1;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    for (int i = 0; i < s.cfg.max_clients; i++) {
        if (s.udp[i].used && s.udp[i].addr.sin_addr.s_addr == from.sin_addr.s_addr &&
            s.udp[i].addr.sin_port == from.sin_port) {
            xSemaphoreGive(s.lock);
            return; /* known client */
        }
        if (!s.udp[i].used && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        free_slot = 0; /* full: replace the oldest slot */
    bool replaced = s.udp[free_slot].used;
    rbx_peer_t old = { .addr = s.udp[free_slot].addr.sin_addr.s_addr,
                       .port = ntohs(s.udp[free_slot].addr.sin_port),
                       .transport = RBX_TRANSPORT_UDP };
    s.udp[free_slot] = (udp_client_t){ .used = true, .addr = from };
    xSemaphoreGive(s.lock);

    if (replaced)
        notify(&old, false);
    rbx_peer_t peer = { .addr = from.sin_addr.s_addr, .port = ntohs(from.sin_port), .transport = RBX_TRANSPORT_UDP };
    notify(&peer, true);
}

static void server_task(void *arg)
{
    (void)arg;
    char buf[64];
    while (s.running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (s.listen_sock >= 0) {
            FD_SET(s.listen_sock, &rfds);
            maxfd = s.listen_sock;
        }
        if (s.udp_sock >= 0) {
            FD_SET(s.udp_sock, &rfds);
            maxfd = MAX(maxfd, s.udp_sock);
        }
        for (int i = 0; i < s.cfg.max_clients; i++) {
            if (s.tcp[i].sock < 0)
                continue;
            if (s.tcp[i].dead) {
                close_tcp(i);
                continue;
            }
            FD_SET(s.tcp[i].sock, &rfds);
            maxfd = MAX(maxfd, s.tcp[i].sock);
        }

        struct timeval tv = { .tv_sec = 0, .tv_usec = 200 * 1000 };
        int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ready <= 0)
            continue;

        if (s.listen_sock >= 0 && FD_ISSET(s.listen_sock, &rfds))
            accept_client();
        if (s.udp_sock >= 0 && FD_ISSET(s.udp_sock, &rfds))
            read_udp();
        for (int i = 0; i < s.cfg.max_clients; i++) {
            if (s.tcp[i].sock < 0 || !FD_ISSET(s.tcp[i].sock, &rfds))
                continue;
            int n = recv(s.tcp[i].sock, buf, sizeof(buf), 0);
            if (n > 0)
                count_acks(buf, n);
            else
                close_tcp(i);
        }
    }

    for (int i = 0; i < s.cfg.max_clients; i++)
        if (s.tcp[i].sock >= 0)
            close_tcp(i);
    if (s.listen_sock >= 0)
        close(s.listen_sock);
    if (s.udp_sock >= 0)
        close(s.udp_sock);
    s.listen_sock = s.udp_sock = -1;
    xSemaphoreGive(s.done);
    vTaskDelete(NULL);
}

esp_err_t rbx_box_server_start(const rbx_box_server_config_t *config)
{
    if (s.running)
        return ESP_ERR_INVALID_STATE;
    if (!config || !config->transports)
        return ESP_ERR_INVALID_ARG;

    s.cfg = *config;
    if (s.cfg.max_clients == 0 || s.cfg.max_clients > CONFIG_RBX_MAX_BOX_CLIENTS)
        s.cfg.max_clients = CONFIG_RBX_MAX_BOX_CLIENTS;
    for (int i = 0; i < CONFIG_RBX_MAX_BOX_CLIENTS; i++) {
        s.tcp[i].sock = -1;
        s.udp[i].used = false;
    }
    s.acks = 0;
    if (!s.lock)
        s.lock = xSemaphoreCreateMutex();
    if (!s.done)
        s.done = xSemaphoreCreateBinary();
    if (!s.lock || !s.done)
        return ESP_ERR_NO_MEM;

    if ((s.cfg.transports & RBX_TRANSPORT_TCP) && (s.listen_sock = open_listener(s.cfg.port)) < 0)
        return ESP_FAIL;
    if ((s.cfg.transports & RBX_TRANSPORT_UDP) && (s.udp_sock = rbx_udp_socket(s.cfg.port)) < 0) {
        if (s.listen_sock >= 0)
            close(s.listen_sock);
        s.listen_sock = -1;
        return ESP_FAIL;
    }

    s.running = true;
    if (xTaskCreate(server_task, "rbx_box_srv", CONFIG_RBX_TASK_STACK, NULL, CONFIG_RBX_TASK_PRIORITY, &s.task) !=
        pdPASS) {
        s.running = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Serving on port %u (%s%s)", s.cfg.port, (s.cfg.transports & RBX_TRANSPORT_TCP) ? "TCP " : "",
             (s.cfg.transports & RBX_TRANSPORT_UDP) ? "UDP" : "");
    return ESP_OK;
}

void rbx_box_server_stop(void)
{
    if (!s.running)
        return;
    s.running = false;
    xSemaphoreTake(s.done, pdMS_TO_TICKS(2000));
}

bool rbx_box_server_running(void)
{
    return s.running;
}

int rbx_box_server_client_count(void)
{
    int n = 0;
    if (!s.lock)
        return 0;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    for (int i = 0; i < s.cfg.max_clients; i++)
        n += (s.tcp[i].sock >= 0 && !s.tcp[i].dead) + s.udp[i].used;
    xSemaphoreGive(s.lock);
    return n;
}

uint32_t rbx_box_server_ack_count(void)
{
    return s.acks;
}

esp_err_t rbx_box_server_send_raw(const char *data, size_t len)
{
    if (!s.running)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    for (int i = 0; i < s.cfg.max_clients; i++) {
        tcp_client_t *c = &s.tcp[i];
        if (c->sock < 0 || c->dead)
            continue;
        size_t off = 0;
        while (off < len) {
            int n = send(c->sock, data + off, len - off, 0);
            if (n <= 0) {
                ESP_LOGW(TAG, "Send failed (errno %d), dropping TCP client", errno);
                c->dead = true; /* the server task closes it */
                break;
            }
            off += n;
        }
    }
    if (s.udp_sock >= 0) {
        for (int i = 0; i < s.cfg.max_clients; i++)
            if (s.udp[i].used)
                sendto(s.udp_sock, data, len, 0, (struct sockaddr *)&s.udp[i].addr, sizeof(s.udp[i].addr));
    }
    xSemaphoreGive(s.lock);
    return ESP_OK;
}

static esp_err_t send_formatted(const char *buf, int len)
{
    return (len > 0) ? rbx_box_server_send_raw(buf, len) : ESP_ERR_INVALID_ARG;
}

esp_err_t rbx_box_server_send_pulse(int64_t s1, int64_t s2, uint64_t ms)
{
    char buf[80];
    return send_formatted(buf, rbx_fmt_pulse(buf, sizeof(buf), s1, s2, ms));
}

esp_err_t rbx_box_server_send_pulse_coords(int64_t count, uint64_t ms, double lat, double lon)
{
    char buf[96];
    return send_formatted(buf, rbx_fmt_pulse_coords(buf, sizeof(buf), count, ms, lat, lon));
}

esp_err_t rbx_box_server_send_button(rbx_msg_type_t button, int64_t s1, int64_t s2)
{
    char buf[64];
    return send_formatted(buf, rbx_fmt_button(buf, sizeof(buf), button, s1, s2));
}

esp_err_t rbx_box_server_send_gps(rbx_gps_kind_t kind, const rbx_gps_fix_t *fix)
{
    char buf[128];
    return send_formatted(buf, rbx_fmt_gps(buf, sizeof(buf), kind, fix));
}
