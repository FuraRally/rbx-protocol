# rbx-protocol (ESP-IDF)

ESP-IDF component implementing the Rabbit rally protocols, **both sides**:

| Role | Header | Transport |
|---|---|---|
| Act as a RabbitBox / Rabbit Connect | `rbx_box_server.h` | TCP and/or UDP 11912 |
| Act as Rabbit 2 towards a box / Rabbit Connect | `rbx_box_client.h` | TCP or UDP 11912 |
| Act as a pilot screen of a main Rabbit 2 phone | `rbx_pilot_client.h` | UDP 7532 |
| Act as the main app serving pilot screens | `rbx_pilot_server.h` | UDP 7532 |
| Box server **+ automatic pilot** to whoever connects | `rbx_link.h` | both |
| Portable framing / encoders / parsers (no ESP-IDF) | `rbx_frame.h` | — |

It supersedes [rbx-protocol-server](https://github.com/FuraRally/rbx-protocol-server).

## Disclaimer

Independent implementation for interoperability; not affiliated with, endorsed by, or connected to Rabbit or its
authors. "Rabbit" may be a trademark of its owner. No original code or assets of the Rabbit apps are included.

## Installation

```yaml
# main/idf_component.yml
dependencies:
  rbx-protocol:
    git: https://github.com/FuraRally/rbx-protocol.git
```

## The idea: box + pilot ("link")

When Rabbit 2 connects to our box port, `rbx_link` subscribes as a pilot screen to that same IP. Rabbit 2 always runs
its pilot server, so the device gets Rabbit's computed values with zero configuration: distance, speed, difference to
ideal, substage, references, chronometer. A display, for example, can then show them.

```c
#include "rbx.h"

static void on_update(const rbx_pilot_state_t *st, uint16_t changed, void *ctx)
{
    if (changed & RBX_TAG_BIT(RBX_TAG_TIME_TO_IDEAL))
        show_difference(st->time_to_ideal_s);         /* > 0 late, < 0 early */
    if (changed & RBX_TAG_BIT(RBX_TAG_SUBSTAGE))
        show_text(rbx_pilot_text(st, RBX_TAG_SUBSTAGE)); /* "V50>E30", as sent */
}

rbx_link_config_t cfg = RBX_LINK_DEFAULT_CONFIG();
cfg.pilot.on_update = on_update;
ESP_ERROR_CHECK(rbx_link_start(&cfg));

/* in the pulse task: */
rbx_box_server_send_pulse(s1_total, s2_total, esp_timer_get_time() / 1000);
```

The pilot client follows the most recent phone that connects. It re-subscribes after `stale_ms` without data, and
reports the link through `on_link`. Callbacks run in the component's task. Use `rbx_pilot_client_get_state()` to read
a consistent copy from elsewhere.

## Other roles

```c
/* Plain box (no pilot), TCP + UDP */
rbx_box_server_config_t box = RBX_BOX_SERVER_DEFAULT_CONFIG();
box.transports = RBX_TRANSPORT_TCP | RBX_TRANSPORT_UDP;
rbx_box_server_start(&box);
rbx_box_server_send_button(RBX_MSG_LAP, s1, s2);
rbx_box_server_send_gps(RBX_GPS_CO, &fix);            /* act as Rabbit Connect */

/* Read a real RabbitBox like the app does */
rbx_box_client_config_t bc = RBX_BOX_CLIENT_DEFAULT_CONFIG();   /* 192.168.4.1:11912 TCP */
bc.on_msg = on_box_msg;                                          /* pulses, buttons, GPS */
rbx_box_client_handle_t h;
rbx_box_client_start(&bc, &h);

/* A standalone pilot screen of a known phone */
rbx_pilot_client_config_t pc = RBX_PILOT_CLIENT_DEFAULT_CONFIG();
pc.main_addr = ipaddr_addr("192.168.100.114");
pc.on_update = on_update;
rbx_pilot_client_start(&pc, &h2);

/* Be the main app for pilot screens (e.g. with the device's own rally data) */
rbx_pilot_server_config_t ps = RBX_PILOT_SERVER_DEFAULT_CONFIG();
rbx_pilot_server_start(&ps);
char rec[40];
rbx_fmt_tag_km_speed(rec, sizeof(rec), km, speed, false);
rbx_pilot_server_set(RBX_TAG_KM_SPEED, rec);   /* the server sends only what changed, every period */
```

## Migrating from rbx-protocol-server

`rbx-protocol-server` is deprecated and this component does not provide its API. Replace:

| Old (`rb.h`) | New |
|---|---|
| `rb_start()` / `rb_stop()` | `rbx_box_server_start(&cfg)` / `rbx_box_server_stop()`, or `rbx_link_start()` / `rbx_link_stop()` for box + pilot |
| `rb_send_frame(&RB_FRAME_PULSE(s1, s2, ms))` | `rbx_box_server_send_pulse(s1, s2, ms)` |
| `rb_send_frame(&RB_FRAME_BUTTON(RB_LAP, s1, s2))` | `rbx_box_server_send_button(RBX_MSG_LAP, s1, s2)` |
| `RB_BUTTON_UP`, `RB_BUTTON_DOWN`, `RB_BUTTON_UP_10`, `RB_BUTTON_DOWN_10`, `RB_LAP`, `RB_CKM` | `RBX_MSG_BT_UP`, `RBX_MSG_BT_DOWN`, `RBX_MSG_BT_UP_10`, `RBX_MSG_BT_DOWN_10`, `RBX_MSG_LAP`, `RBX_MSG_CKM` |

## Notes and limits
- **UDP 7532 is needed by the pilot client and by the pilot server.** Only one of them can run on a device.
- The real app answers pilots only on port 7532 and keeps every pilot registered until it restarts.
  Re-subscribing is harmless.
- Box numbers in Rabbit 2 select the transport: 1000–1099 → TCP to `192.168.4.1:11912`; 1051 and 1100–1199 → UDP
  to `10.10.10.1`; 1200+ → UDP to `192.168.4.1`. Naming the AP `RabbitBox<number>` lets the app learn the number.
- Number formatting uses `printf` (binary rounding). Rabbit rounds decimals half away from zero, so a value exactly on
  a half (e.g. `1.0365` km) can differ in the last digit.
- BLE (pilot over GATT `fff0/fff1`, BLE boxes) is not implemented yet.

## Tests

Host tests of the portable layer (no ESP-IDF needed):

```sh
cmake -S test/host -B test/host/build && cmake --build test/host/build && ctest --test-dir test/host/build
```

Example app: [examples/box_with_pilot](examples/box_with_pilot/main/main.c), a simulated box that logs Rabbit's
data. It builds for ESP32-S3 and ESP32-C3.

### On hardware, with the Rabbit 2 app

Flash the example, connect the phone to the `RabbitBox1001` Wi-Fi, and in Rabbit 2 → Connections enter box number
`1001`. The ESP should:
- serve pulses (the odometer advances at the simulated speed);
- subscribe to the phone as a pilot as soon as Rabbit connects;
- log `Rabbit: km … diff … substage …` with the values computed by the app.

## License

GPL-3.0, see `LICENSE`.
