/**
 * @file rbx_net.c
 * @brief Private socket helpers.
 */
#include "rbx_net.h"

#include "esp_log.h"
#include "lwip/inet.h"
#include "rbx_types.h"

static const char *TAG = "rbx_net";

const char *rbx_peer_ip_str(const rbx_peer_t *peer, char *buf, size_t size)
{
    struct in_addr a = { .s_addr = peer->addr };
    inet_ntoa_r(a, buf, size);
    return buf;
}

int rbx_udp_socket(uint16_t port)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create UDP socket: errno %d", errno);
        return -1;
    }
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "Unable to bind UDP port %u: errno %d", port, errno);
        close(sock);
        return -1;
    }
    return sock;
}

void rbx_set_timeouts(int sock, uint32_t rcv_ms, uint32_t snd_ms)
{
    if (rcv_ms) {
        struct timeval tv = { .tv_sec = rcv_ms / 1000, .tv_usec = (rcv_ms % 1000) * 1000 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    if (snd_ms) {
        struct timeval tv = { .tv_sec = snd_ms / 1000, .tv_usec = (snd_ms % 1000) * 1000 };
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
}
