/**
 * @file rbx_net.h
 * @brief Private socket helpers shared by the rbx roles.
 */
#ifndef RBX_NET_H
#define RBX_NET_H

#include <stdint.h>

#include "esp_timer.h"
#include "lwip/sockets.h"

/** Milliseconds since boot. */
static inline int64_t rbx_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/** UDP socket bound to INADDR_ANY:port (0 = any), with SO_REUSEADDR; -1 on error. */
int rbx_udp_socket(uint16_t port);

/** Set SO_RCVTIMEO / SO_SNDTIMEO in milliseconds. */
void rbx_set_timeouts(int sock, uint32_t rcv_ms, uint32_t snd_ms);

#endif /* RBX_NET_H */
