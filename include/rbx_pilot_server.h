/**
 * @file rbx_pilot_server.h
 * @brief Act as the main Rabbit 2 app towards pilot screens (UDP 7532), with the app's send rules:
 * any datagram registers its IP (answers go to port 7532), every datagram schedules a full resend
 * 2 s later, tag 6 is repeated 2 s after a change, only changed tags are sent, one datagram per period.
 *
 * The application sets the current records (built with the rbx_fmt_tag_* helpers); the server
 * decides what to send.
 */
#ifndef RBX_PILOT_SERVER_H
#define RBX_PILOT_SERVER_H

#include "sdkconfig.h"
#include "rbx_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RBX_PILOT_PAGE_NORMAL = 0,  /**< tags 1 5 6 7 2 0 */
    RBX_PILOT_PAGE_TABLAS,      /**< tags 1 4 6 7 2 0 */
    RBX_PILOT_PAGE_DOUBLE_TRIP, /**< tags 8 9 0 */
} rbx_pilot_page_t;

typedef void (*rbx_pilot_registered_cb_t)(const rbx_peer_t *pilot, void *ctx);

typedef struct {
    uint16_t port;          /**< default RBX_PILOT_PORT */
    uint16_t period_ms;     /**< 30 = "Fast Rabbit Pilot", 100 = normal */
    uint8_t max_pilots;     /**< <= CONFIG_RBX_MAX_PILOTS */
    bool reply_to_source;   /**< answer to the source port instead of 7532 (tests only) */
    rbx_pilot_registered_cb_t on_pilot;
    void *ctx;
} rbx_pilot_server_config_t;

#define RBX_PILOT_SERVER_DEFAULT_CONFIG()                                                                              \
    {                                                                                                                  \
        .port = RBX_PILOT_PORT, .period_ms = 30, .max_pilots = CONFIG_RBX_MAX_PILOTS, .reply_to_source = false,        \
        .on_pilot = NULL, .ctx = NULL,                                                                                 \
    }

esp_err_t rbx_pilot_server_start(const rbx_pilot_server_config_t *config);
void rbx_pilot_server_stop(void);
int rbx_pilot_server_pilot_count(void);

/** Set the current record of a tag, e.g. from rbx_fmt_tag_km_speed(). Pass NULL to clear it. */
esp_err_t rbx_pilot_server_set(char tag, const char *record);
void rbx_pilot_server_set_page(rbx_pilot_page_t page);

#ifdef __cplusplus
}
#endif

#endif /* RBX_PILOT_SERVER_H */
