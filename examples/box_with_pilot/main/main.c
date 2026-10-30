/*
 * box_with_pilot: a simulated RabbitBox that also listens to Rabbit's computed data.
 *
 * 1. Join the phone to the Wi-Fi "RabbitBox1001" and, in Rabbit 2 → Connections, use box number 1001.
 * 2. The ESP streams simulated pulses (1 pulse = 1 m, i.e. app calibration R = 1).
 * 3. As soon as Rabbit connects, the ESP subscribes as a pilot screen to the phone and logs the
 *    distance, speed, difference to ideal and substage that Rabbit computes.
 */
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "rbx.h"

static const char *TAG = "example";

#define PULSE_PERIOD_MS 200 /* 5 messages per second, like RabbitTest */

static void wifi_ap_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap(); /* 192.168.4.1, DHCP server on */
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    wifi_config_t cfg = { .ap = { .channel = 1, .max_connection = 4 } };
    strlcpy((char *)cfg.ap.ssid, CONFIG_EXAMPLE_AP_SSID, sizeof(cfg.ap.ssid));
    strlcpy((char *)cfg.ap.password, CONFIG_EXAMPLE_AP_PASSWORD, sizeof(cfg.ap.password));
    cfg.ap.ssid_len = strlen(CONFIG_EXAMPLE_AP_SSID);
    cfg.ap.authmode = strlen(CONFIG_EXAMPLE_AP_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "AP \"%s\" up at 192.168.4.1", CONFIG_EXAMPLE_AP_SSID);
}

static void on_pilot_update(const rbx_pilot_state_t *st, uint16_t changed, void *ctx)
{
    (void)ctx;
    if (changed & RBX_TAG_BIT(RBX_TAG_SUBSTAGE))
        ESP_LOGI(TAG, "substage %s", rbx_pilot_text(st, RBX_TAG_SUBSTAGE));
    if (changed & RBX_TAG_BIT(RBX_TAG_REFERENCE))
        ESP_LOGI(TAG, "reference %ld", (long)st->reference);
}

static void on_pilot_link(bool receiving, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "Rabbit pilot data %s", receiving ? "arriving" : "lost");
}

static void on_box_client(const rbx_peer_t *peer, bool connected, void *ctx)
{
    (void)ctx;
    char ip[16];
    ESP_LOGI(TAG, "Rabbit %s %s", rbx_peer_ip_str(peer, ip, sizeof(ip)), connected ? "connected" : "left");
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    wifi_ap_start();

    rbx_link_config_t link = RBX_LINK_DEFAULT_CONFIG();
    link.box.on_client = on_box_client;
    link.pilot.on_update = on_pilot_update;
    link.pilot.on_link = on_pilot_link;
    ESP_ERROR_CHECK(rbx_link_start(&link));

    double meters = 0;
    int64_t last_log = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(PULSE_PERIOD_MS));
        meters += CONFIG_EXAMPLE_SIM_SPEED_KMH / 3.6 * PULSE_PERIOD_MS / 1000.0;
        uint64_t ms = esp_timer_get_time() / 1000;
        rbx_box_server_send_pulse((int64_t)meters, (int64_t)meters, ms);

        rbx_pilot_client_handle_t pilot = rbx_link_pilot();
        if (pilot && rbx_pilot_client_receiving(pilot) && ms - last_log >= 1000) {
            rbx_pilot_state_t st;
            rbx_pilot_client_get_state(pilot, &st);
            ESP_LOGI(TAG, "Rabbit: km %.3f  %.1f km/h  diff %s s  chrono %ld s  to substage end %.3f km", st.km,
                     st.speed_kmh, rbx_pilot_text(&st, RBX_TAG_TIME_TO_IDEAL), (long)st.chrono_s,
                     st.km_to_substage_end);
            last_log = ms;
        }
    }
}
