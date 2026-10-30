/**
 * @file rbx_link.c
 * @brief Box server + automatic pilot client to the IP of whoever connects to the box.
 */
#include "esp_log.h"

#include "rbx_link.h"

static const char *TAG = "rbx_link";

static struct {
    rbx_link_config_t cfg;
    rbx_pilot_client_handle_t pilot;
} s;

static void on_box_client(const rbx_peer_t *peer, bool connected, void *ctx)
{
    (void)ctx;
    if (connected) {
        char ip[16];
        rbx_peer_ip_str(peer, ip, sizeof(ip));
        if (!s.pilot) {
            rbx_pilot_client_config_t pc = s.cfg.pilot;
            pc.main_addr = peer->addr;
            if (rbx_pilot_client_start(&pc, &s.pilot) == ESP_OK)
                ESP_LOGI(TAG, "Pilot client subscribing to %s", ip);
            else
                ESP_LOGE(TAG, "Unable to start the pilot client (is UDP %u in use?)", pc.local_port);
        }
        else if (rbx_pilot_client_target(s.pilot) != peer->addr) {
            ESP_LOGI(TAG, "Pilot client following new box client %s", ip);
            rbx_pilot_client_set_target(s.pilot, peer->addr);
        }
    }
    /* On disconnect the pilot keeps its target: the phone keeps streaming, and usually reconnects. */
    if (s.cfg.box.on_client)
        s.cfg.box.on_client(peer, connected, s.cfg.box.ctx);
}

esp_err_t rbx_link_start(const rbx_link_config_t *config)
{
    if (!config)
        return ESP_ERR_INVALID_ARG;
    s.cfg = *config;
    rbx_box_server_config_t box = config->box;
    box.on_client = on_box_client;
    box.ctx = NULL;
    return rbx_box_server_start(&box);
}

void rbx_link_stop(void)
{
    rbx_box_server_stop();
    rbx_pilot_client_stop(s.pilot);
    s.pilot = NULL;
}

rbx_pilot_client_handle_t rbx_link_pilot(void)
{
    return s.pilot;
}
