/**
 * @file rbx_frame.h
 * @brief Portable (no ESP-IDF) framing, encoding and parsing for the Rabbit text protocols.
 *
 * - Box stream (RabbitBox / Rabbit Connect -> Rabbit 2): `pulse=`, `pc=`, GPS lines, buttons.
 * - Pilot stream (Rabbit 2 -> pilot screens): one-character tags `0`..`9`.
 *
 * Every record ends with '|'. CR/LF are ignored.
 */
#ifndef RBX_FRAME_H
#define RBX_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RBX_BOX_PORT   11912 /**< RabbitBox / Rabbit Connect port (TCP and UDP) */
#define RBX_PILOT_PORT 7532  /**< Pilot-screen port (UDP, both sides) */

#define RBX_RECORD_MAX 128 /**< Longest record kept by the splitter; longer ones are dropped */

/* ------------------------------------------------------------------ record splitter */

typedef void (*rbx_record_cb_t)(const char *record, size_t len, void *ctx);

/** Reassembles '|'-terminated records from arbitrary chunks (TCP reads, datagrams, notifications). */
typedef struct {
    char buf[RBX_RECORD_MAX];
    size_t len;
    bool discard; /**< current record overflowed: drop it up to the next '|' */
} rbx_splitter_t;

void rbx_splitter_init(rbx_splitter_t *s);

/**
 * @brief Feed received bytes; calls cb once per complete, non-blank record (without the '|').
 * The record is NUL-terminated and valid only during the callback.
 */
void rbx_splitter_feed(rbx_splitter_t *s, const char *data, size_t len, rbx_record_cb_t cb, void *ctx);

/* ------------------------------------------------------------------ box stream */

typedef enum {
    RBX_MSG_INVALID = 0,
    RBX_MSG_PULSE,           /**< pulse=s1;s2;ms;x  or  pulse=count;ms;x */
    RBX_MSG_PULSE_COORDS,    /**< pc=count;ms;lon;lat */
    RBX_MSG_PULSE_ENCRYPTED, /**< pe=...  recognised, not decoded */
    RBX_MSG_GPS,             /**< pg= pg1= pg2= co= ci=  (see gps_kind) */
    RBX_MSG_BT_UP,
    RBX_MSG_BT_DOWN,
    RBX_MSG_BT_UP_10,
    RBX_MSG_BT_DOWN_10,
    RBX_MSG_LAP,
    RBX_MSG_CKM,
} rbx_msg_type_t;

typedef enum {
    RBX_GPS_PG = 0, /**< GPS odometer for both sensors */
    RBX_GPS_PG1,    /**< sensor 1 only */
    RBX_GPS_PG2,    /**< sensor 2 only */
    RBX_GPS_CO,     /**< Rabbit Connect GPS (behaves like pg) */
    RBX_GPS_CI,     /**< position only, no distance */
} rbx_gps_kind_t;

/** One GPS fix as carried on the wire. */
typedef struct {
    uint16_t year;
    uint8_t month, day, hour, minute, second, tenth; /**< UTC */
    double lat, lon;                                 /**< decimal degrees */
    int32_t speed_kmh;
    int32_t heading_deg;
    float accuracy; /**< meters (HDOP from Rabbit Connect NMEA sources) */
    float altitude; /**< meters, optional (ignored by Rabbit 2) */
    bool has_altitude;
} rbx_gps_fix_t;

typedef struct {
    rbx_msg_type_t type;
    int64_t s1, s2;   /**< pulse counters (or the numbers carried by a button) */
    uint64_t ms;      /**< box clock, pulse messages */
    double lat, lon;  /**< pc= */
    rbx_gps_kind_t gps_kind;
    rbx_gps_fix_t fix; /**< GPS messages */
} rbx_box_msg_t;

/**
 * @brief Parse one record the way Rabbit 2 does.
 * @return true if the app would accept it (out filled), false if it would drop it.
 */
bool rbx_parse_box_record(const char *record, size_t len, rbx_box_msg_t *out);

/** Keyword of a message type ("pulse", "btUp10"...), or NULL. GPS: pass the kind via rbx_gps_keyword(). */
const char *rbx_msg_keyword(rbx_msg_type_t type);
const char *rbx_gps_keyword(rbx_gps_kind_t kind);

/* Encoders. All return the length written (excluding NUL) or -1 if buf is too small. Each ends with "|\n". */
int rbx_fmt_pulse(char *buf, size_t size, int64_t s1, int64_t s2, uint64_t ms);
int rbx_fmt_pulse_coords(char *buf, size_t size, int64_t count, uint64_t ms, double lat, double lon);
int rbx_fmt_button(char *buf, size_t size, rbx_msg_type_t type, int64_t s1, int64_t s2);
int rbx_fmt_gps(char *buf, size_t size, rbx_gps_kind_t kind, const rbx_gps_fix_t *fix);

/* ------------------------------------------------------------------ pilot stream */

#define RBX_PILOT_TEXT_MAX 32 /**< kept per tag: the value exactly as received */

/** Bit for a tag character in the masks below: RBX_TAG_BIT('5'). */
#define RBX_TAG_BIT(tag) ((uint16_t)(1u << ((tag) - '0')))

typedef enum {
    RBX_TAG_CLOCK = '0',         /**< HH:mm:ss;[-]hh:mm:ss  clock ; chronometer */
    RBX_TAG_KM_SPEED = '1',      /**< km;speed */
    RBX_TAG_REGRESSIVES = '2',   /**< km to substage end ; km to next reference */
    RBX_TAG_TABLAS = '4',        /**< text (Tablas page, replaces 5) */
    RBX_TAG_TIME_TO_IDEAL = '5', /**< seconds, positive = late */
    RBX_TAG_SUBSTAGE = '6',      /**< "V50>E30" (localized letters, may contain ',') */
    RBX_TAG_REFERENCE = '7',     /**< reference number or empty */
    RBX_TAG_TRIP1 = '8',         /**< km;speed;average (double-trip page) */
    RBX_TAG_TRIP2 = '9',
} rbx_pilot_tag_t;

/** Everything a pilot screen knows. Text fields keep the main app's formatting for display. */
typedef struct {
    uint16_t present;                       /**< RBX_TAG_BIT of every tag received at least once */
    char raw[10][RBX_PILOT_TEXT_MAX];       /**< value of each tag as received ('0'..'9') */
    uint8_t clock_h, clock_m, clock_s;      /**< tag 0: main phone clock */
    int32_t chrono_s;                       /**< tag 0: stage chronometer, negative before the start */
    double km, speed_kmh;                   /**< tag 1 */
    double km_to_substage_end;              /**< tag 2 */
    double km_to_next_ref;                  /**< tag 2 */
    double time_to_ideal_s;                 /**< tag 5: positive = late, negative = early */
    int32_t reference;                      /**< tag 7: -1 = none */
    double trip_km[2], trip_speed_kmh[2], trip_average_kmh[2]; /**< tags 8/9 */
} rbx_pilot_state_t;

void rbx_pilot_state_init(rbx_pilot_state_t *st);

/**
 * @brief Apply one pilot record ("5-1.25", without '|').
 * @return RBX_TAG_BIT of the tag if its value changed, 0 if unchanged or not understood.
 */
uint16_t rbx_pilot_apply(rbx_pilot_state_t *st, const char *record, size_t len);

/** Text of a tag (substage text, tabla...) as received, "" if never received. */
static inline const char *rbx_pilot_text(const rbx_pilot_state_t *st, char tag)
{
    return (tag >= '0' && tag <= '9') ? st->raw[tag - '0'] : "";
}

/* Pilot record encoders (for a pilot server). Same formats as Rabbit 2. Return length or -1. */
int rbx_fmt_tag_clock(char *buf, size_t size, int h, int m, int s, int32_t chrono_s);
int rbx_fmt_tag_km_speed(char *buf, size_t size, double km, double speed_kmh, bool work10m);
int rbx_fmt_tag_regressives(char *buf, size_t size, double to_substage_end, double to_next_ref, bool work10m);
int rbx_fmt_tag_text(char *buf, size_t size, char tag, const char *text); /**< tags 4 and 6 */
int rbx_fmt_tag_time_to_ideal(char *buf, size_t size, double seconds);
int rbx_fmt_tag_reference(char *buf, size_t size, int32_t reference); /**< < 0 = none */
int rbx_fmt_tag_trip(char *buf, size_t size, int trip, double km, double speed_kmh, double average_kmh, bool work10m);

#ifdef __cplusplus
}
#endif

#endif /* RBX_FRAME_H */
