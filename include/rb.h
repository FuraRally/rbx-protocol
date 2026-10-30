/**
 * @file rb.h
 * @brief Legacy rbx-protocol-server API, kept so existing firmware (FuraZero) builds unchanged.
 *
 * rb_start() starts the box server on TCP 11912 (and, with CONFIG_RBX_LEGACY_AUTO_PILOT, the automatic
 * pilot client). New code should use rbx_box_server.h / rbx_link.h directly.
 */
#ifndef RB_H
#define RB_H

#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RB_FRAME_PULSE(sensor1, sensor2, time)                                                                         \
    {                                                                                                                  \
        .type = RB_PULSE, .s1 = sensor1, .s2 = sensor2, .timestamp = time,                                             \
    }

#define RB_FRAME_BUTTON(buttonType, sensor1, sensor2)                                                                  \
    {                                                                                                                  \
        .type = buttonType, .s1 = sensor1, .s2 = sensor2, .timestamp = 0,                                              \
    }

typedef enum {
    RB_PULSE,
    RB_BUTTON_UP,
    RB_BUTTON_DOWN,
    RB_BUTTON_UP_10,
    RB_BUTTON_DOWN_10,
    RB_LAP,
    RB_CKM,
    MAX_RB_MSG_TYPE,
} rb_msg_type_t;

typedef struct {
    rb_msg_type_t type;
    int s1;
    int s2;
    unsigned int timestamp; /* Only for pulse */
} rb_frame_t;

void rb_start(void);
void rb_stop(void);

rb_frame_t create_pulse_frame(int s1, int s2, unsigned int timestamp);
rb_frame_t create_button_frame(rb_msg_type_t type, int s1, int s2);

esp_err_t rb_send_frame(const rb_frame_t *frame);

#endif /* RB_H */
