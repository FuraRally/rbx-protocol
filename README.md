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
| Legacy `rbx-protocol-server` API | `rb.h` | TCP 11912 |

It supersedes [rbx-protocol-server](../rbx-protocol-server/). The protocol specs (from captures, live tests and analysis
of the app) are in [rabbit-protocol/docs/protocols](../rabbit-protocol/docs/protocols/README.md).

## Disclaimer

Independent implementation for interoperability; not affiliated with, endorsed by, or connected to Rabbit or its
authors. "Rabbit" may be a trademark of its owner. No original code or assets of the Rabbit apps are included.

## Installation

```yaml
# main/idf_component.yml
dependencies:
  rbx-protocol:
    path: ../../rbx-protocol          # or git: https://github.com/FuraRally/rbx-protocol.git
```

## The idea: box + pilot ("link")

When Rabbit 2 connects to our box port, `rbx_link` subscribes as a pilot screen to that same IP. Rabbit 2 always runs
its pilot server, so the device gets Rabbit's computed values with zero configuration: distance, speed, difference to
ideal, substage, references, chronometer. A display or the FuraStrip can then show them.

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

/* Be the main app for pilot screens (e.g. FuraZero's own rally data) */
rbx_pilot_server_config_t ps = RBX_PILOT_SERVER_DEFAULT_CONFIG();
rbx_pilot_server_start(&ps);
char rec[40];
rbx_fmt_tag_km_speed(rec, sizeof(rec), km, speed, false);
rbx_pilot_server_set(RBX_TAG_KM_SPEED, rec);   /* the server sends only what changed, every period */
```

## Migrating from rbx-protocol-server

Swap the dependency; `#include "rb.h"`, `rb_start()`, `RB_FRAME_*` and `rb_send_frame()` keep working.
With `CONFIG_RBX_LEGACY_AUTO_PILOT=y`, `rb_start()` starts the link, so you also get `rbx_link_pilot()`.

Changes compared to the old server:
- One task with `select()` instead of one task per client.
- Sends have a timeout (`send_timeout_ms`, default 100 ms). A client that can't keep up is dropped instead of
  blocking the pulse task.
- The 0x01 acks from Rabbit 2 are counted (`rbx_box_server_ack_count()`).
- Optional UDP transport.
- `rb_send_frame()` returns `ESP_ERR_INVALID_STATE` if the server isn't running.

## Notes and limits
- **UDP 7532 is needed by the pilot client and by the pilot server.** Only one of them can run on a device.
- The real app answers pilots only on port 7532 and keeps every pilot registered until it restarts.
  Re-subscribing is harmless.
- Box numbers in Rabbit 2: an AP named `RabbitBox10xx` at `192.168.4.1` with box number 1000–1099 selects TCP.
  See [box-number.md](../rabbit-protocol/docs/protocols/rabbitbox/box-number.md).
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

### On hardware, with rabbitlab (no phone needed)

Flash the example, join the PC to the `RabbitBox1001` Wi-Fi, then from `rabbit-protocol/rabbitlab/`:

```sh
./rlab --control 47531 pilot main --speed 50 &   # the PC plays the main app on UDP 7532
./rlab client 192.168.4.1                         # the PC plays Rabbit 2 connecting to the box
```

The ESP should:
- serve pulses to `rlab client` (km increasing at the simulated speed, acks counted);
- subscribe to the PC as a pilot as soon as `rlab client` connects;
- log `Rabbit: km … diff … substage …` coming from `rlab pilot main`.

Then the same with the real phone: Rabbit 2 → Connections → box number 1001.

## License

GPL-3.0, see `LICENSE`.
