/**
 * @file rbx_box_client.h
 * @brief Act as Rabbit 2 towards a RabbitBox or Rabbit Connect (TCP or UDP 11912).
 *
 * TCP: connects and writes 0x01 after every read, like the app. UDP: binds a local port, sends 0x01
 * hellos until data arrives, and acknowledges each datagram to its sender. Reconnects automatically.
 */
#ifndef RBX_BOX_CLIENT_H
#define RBX_BOX_CLIENT_H

#include "sdkconfig.h"
#include "rbx_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rbx_box_client *rbx_box_client_handle_t;

/** Called from the client task for every record the app would accept. */
typedef void (*rbx_box_msg_cb_t)(const rbx_box_msg_t *msg, void *ctx);
typedef void (*rbx_link_state_cb_t)(bool connected, void *ctx);

typedef struct {
    char host[16];             /**< box IP, e.g. "192.168.4.1" */
    uint16_t port;             /**< default RBX_BOX_PORT */
    rbx_transport_t transport; /**< RBX_TRANSPORT_TCP or RBX_TRANSPORT_UDP */
    uint16_t local_port;       /**< UDP only: local port (the app uses 11912; 0 = any) */
    bool ack;                  /**< send 0x01 acks (default true) */
    uint32_t reconnect_ms;     /**< delay between attempts */
    uint32_t silence_ms;       /**< UDP: re-send hellos after this long without data */
    rbx_box_msg_cb_t on_msg;
    rbx_link_state_cb_t on_state;
    void *ctx;
} rbx_box_client_config_t;

#define RBX_BOX_CLIENT_DEFAULT_CONFIG()                                                                                \
    {                                                                                                                  \
        .host = "192.168.4.1", .port = RBX_BOX_PORT, .transport = RBX_TRANSPORT_TCP, .local_port = RBX_BOX_PORT,       \
        .ack = true, .reconnect_ms = 3000, .silence_ms = 3000, .on_msg = NULL, .on_state = NULL, .ctx = NULL,          \
    }

esp_err_t rbx_box_client_start(const rbx_box_client_config_t *config, rbx_box_client_handle_t *out);
void rbx_box_client_stop(rbx_box_client_handle_t client);
bool rbx_box_client_connected(rbx_box_client_handle_t client);

#ifdef __cplusplus
}
#endif

#endif /* RBX_BOX_CLIENT_H */
