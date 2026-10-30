/**
 * @file rbx_link.h
 * @brief Box server + automatic pilot client.
 *
 * When Rabbit 2 connects to our box port, we also subscribe as a pilot screen to that same IP, so the
 * device receives the values Rabbit computes (distance, difference to ideal, substage, references...).
 * If another phone connects later, the pilot client follows the most recent one.
 */
#ifndef RBX_LINK_H
#define RBX_LINK_H

#include "rbx_box_server.h"
#include "rbx_pilot_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    rbx_box_server_config_t box;     /**< its on_client is still called */
    rbx_pilot_client_config_t pilot; /**< main_addr is filled automatically */
} rbx_link_config_t;

#define RBX_LINK_DEFAULT_CONFIG()                                                                                      \
    {                                                                                                                  \
        .box = RBX_BOX_SERVER_DEFAULT_CONFIG(), .pilot = RBX_PILOT_CLIENT_DEFAULT_CONFIG(),                            \
    }

esp_err_t rbx_link_start(const rbx_link_config_t *config);
void rbx_link_stop(void);

/** The pilot client (NULL until the first box client connected). */
rbx_pilot_client_handle_t rbx_link_pilot(void);

#ifdef __cplusplus
}
#endif

#endif /* RBX_LINK_H */
