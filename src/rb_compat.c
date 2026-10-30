/**
 * @file rb_compat.c
 * @brief Legacy rbx-protocol-server API on top of the box server (and optionally the link).
 */
#include "esp_log.h"
#include "sdkconfig.h"

#include "rb.h"
#include "rbx_box_server.h"
#include "rbx_link.h"

static const char *TAG = "rb";

static const rbx_msg_type_t BUTTON_MAP[MAX_RB_MSG_TYPE] = {
    [RB_BUTTON_UP] = RBX_MSG_BT_UP,     [RB_BUTTON_DOWN] = RBX_MSG_BT_DOWN, [RB_BUTTON_UP_10] = RBX_MSG_BT_UP_10,
    [RB_BUTTON_DOWN_10] = RBX_MSG_BT_DOWN_10, [RB_LAP] = RBX_MSG_LAP,       [RB_CKM] = RBX_MSG_CKM,
};

void rb_start(void)
{
#if CONFIG_RBX_LEGACY_AUTO_PILOT
    rbx_link_config_t cfg = RBX_LINK_DEFAULT_CONFIG();
    esp_err_t err = rbx_link_start(&cfg);
#else
    rbx_box_server_config_t cfg = RBX_BOX_SERVER_DEFAULT_CONFIG();
    esp_err_t err = rbx_box_server_start(&cfg);
#endif
    if (err != ESP_OK)
        ESP_LOGE(TAG, "Unable to start the box server: %s", esp_err_to_name(err));
}

void rb_stop(void)
{
#if CONFIG_RBX_LEGACY_AUTO_PILOT
    rbx_link_stop();
#else
    rbx_box_server_stop();
#endif
}

rb_frame_t create_pulse_frame(int s1, int s2, unsigned int timestamp)
{
    rb_frame_t frame = RB_FRAME_PULSE(s1, s2, timestamp);
    return frame;
}

rb_frame_t create_button_frame(rb_msg_type_t type, int s1, int s2)
{
    rb_frame_t frame = RB_FRAME_BUTTON(type, s1, s2);
    return frame;
}

esp_err_t rb_send_frame(const rb_frame_t *frame)
{
    if (!frame || frame->type >= MAX_RB_MSG_TYPE)
        return ESP_ERR_INVALID_ARG;
    if (!rbx_box_server_running())
        return ESP_ERR_INVALID_STATE;
    if (frame->type == RB_PULSE)
        return rbx_box_server_send_pulse(frame->s1, frame->s2, frame->timestamp);
    return rbx_box_server_send_button(BUTTON_MAP[frame->type], frame->s1, frame->s2);
}
