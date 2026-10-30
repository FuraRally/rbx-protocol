/**
 * @file rbx_box_server.h
 * @brief Act as a RabbitBox or Rabbit Connect: serve the box stream on TCP and/or UDP (port 11912).
 *
 * Single instance per device. Sending functions are thread-safe and never block more than the
 * configured send timeout per client; a client that can't keep up is dropped.
 */
#ifndef RBX_BOX_SERVER_H
#define RBX_BOX_SERVER_H

#include "sdkconfig.h"
#include "rbx_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Called from the server task when a client connects (TCP accept / first UDP datagram) or goes away. */
typedef void (*rbx_box_client_event_cb_t)(const rbx_peer_t *peer, bool connected, void *ctx);

typedef struct {
    uint16_t port;              /**< default RBX_BOX_PORT */
    uint8_t transports;         /**< RBX_TRANSPORT_TCP and/or RBX_TRANSPORT_UDP */
    uint8_t max_clients;        /**< per transport, <= CONFIG_RBX_MAX_BOX_CLIENTS */
    uint16_t send_timeout_ms;   /**< per client and send */
    rbx_box_client_event_cb_t on_client;
    void *ctx;
} rbx_box_server_config_t;

#define RBX_BOX_SERVER_DEFAULT_CONFIG()                                                                                \
    {                                                                                                                  \
        .port = RBX_BOX_PORT, .transports = RBX_TRANSPORT_TCP, .max_clients = CONFIG_RBX_MAX_BOX_CLIENTS,              \
        .send_timeout_ms = 100, .on_client = NULL, .ctx = NULL,                                                        \
    }

esp_err_t rbx_box_server_start(const rbx_box_server_config_t *config);
void rbx_box_server_stop(void);
bool rbx_box_server_running(void);

/** Number of connected clients (TCP + registered UDP). */
int rbx_box_server_client_count(void);

/** Bytes 0x01 received from clients (Rabbit 2 acknowledges every read over TCP). */
uint32_t rbx_box_server_ack_count(void);

esp_err_t rbx_box_server_send_pulse(int64_t s1, int64_t s2, uint64_t ms);
esp_err_t rbx_box_server_send_pulse_coords(int64_t count, uint64_t ms, double lat, double lon);
esp_err_t rbx_box_server_send_button(rbx_msg_type_t button, int64_t s1, int64_t s2);
esp_err_t rbx_box_server_send_gps(rbx_gps_kind_t kind, const rbx_gps_fix_t *fix);
/** Send preformatted records (must end with '|', optionally "\n"). */
esp_err_t rbx_box_server_send_raw(const char *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* RBX_BOX_SERVER_H */
