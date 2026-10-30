/**
 * @file rbx_types.h
 * @brief Types shared by the rbx-protocol ESP-IDF roles.
 */
#ifndef RBX_TYPES_H
#define RBX_TYPES_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "rbx_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RBX_TRANSPORT_TCP = 1 << 0,
    RBX_TRANSPORT_UDP = 1 << 1,
} rbx_transport_t;

/** A remote endpoint. addr is an IPv4 address in network byte order (as in struct in_addr). */
typedef struct {
    uint32_t addr;
    uint16_t port;
    rbx_transport_t transport;
} rbx_peer_t;

/** Format peer->addr as dotted quad into buf (>= 16 bytes); returns buf. */
const char *rbx_peer_ip_str(const rbx_peer_t *peer, char *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* RBX_TYPES_H */
