/**
 * @file rbx_pilot_client.h
 * @brief Act as a pilot screen: subscribe to a main Rabbit 2 phone and decode its tags (UDP 7532).
 *
 * The main app registers the *IP* of whoever sends it a datagram and always answers to port 7532,
 * so the local port must be 7532. Re-subscribing is harmless (it also triggers a full resend).
 */
#ifndef RBX_PILOT_CLIENT_H
#define RBX_PILOT_CLIENT_H

#include "sdkconfig.h"
#include "rbx_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rbx_pilot_client *rbx_pilot_client_handle_t;

/**
 * Called from the client task after each datagram that changed something.
 * changed = RBX_TAG_BIT mask of the tags whose value changed. The state is valid during the call.
 */
typedef void (*rbx_pilot_update_cb_t)(const rbx_pilot_state_t *state, uint16_t changed, void *ctx);
/** receiving = data arriving; false after `stale_ms` without data. */
typedef void (*rbx_pilot_link_cb_t)(bool receiving, void *ctx);

typedef struct {
    uint32_t main_addr;        /**< main phone IPv4 (network order); 0 = none yet, see rbx_pilot_client_set_target */
    uint16_t local_port;       /**< must be RBX_PILOT_PORT for the real app */
    uint16_t hello_ms;         /**< interval between 0x01 hellos while nothing arrives (app: 500) */
    uint32_t stale_ms;         /**< no data for this long: report not receiving and re-subscribe */
    rbx_pilot_update_cb_t on_update;
    rbx_pilot_link_cb_t on_link;
    void *ctx;
} rbx_pilot_client_config_t;

#define RBX_PILOT_CLIENT_DEFAULT_CONFIG()                                                                              \
    {                                                                                                                  \
        .main_addr = 0, .local_port = RBX_PILOT_PORT, .hello_ms = 500, .stale_ms = 3000, .on_update = NULL,            \
        .on_link = NULL, .ctx = NULL,                                                                                  \
    }

esp_err_t rbx_pilot_client_start(const rbx_pilot_client_config_t *config, rbx_pilot_client_handle_t *out);
void rbx_pilot_client_stop(rbx_pilot_client_handle_t client);

/** Point the client at another main phone (e.g. the IP that just connected to the box). */
void rbx_pilot_client_set_target(rbx_pilot_client_handle_t client, uint32_t main_addr);
uint32_t rbx_pilot_client_target(rbx_pilot_client_handle_t client);

/** Copy the latest state (thread-safe). */
void rbx_pilot_client_get_state(rbx_pilot_client_handle_t client, rbx_pilot_state_t *out);
bool rbx_pilot_client_receiving(rbx_pilot_client_handle_t client);

#ifdef __cplusplus
}
#endif

#endif /* RBX_PILOT_CLIENT_H */
