/**
 * @file rbx_pilot_state.c
 * @brief Pilot-stream tag parsing and encoding. Portable C99, no ESP-IDF.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rbx_frame.h"

void rbx_pilot_state_init(rbx_pilot_state_t *st)
{
    memset(st, 0, sizeof(*st));
    st->reference = -1;
}

/** Parse "a;b[;c]" doubles; returns the number parsed. */
static int parse_doubles(const char *v, double *out, int max)
{
    int n = 0;
    const char *p = v;
    while (n < max) {
        char *end;
        double d = strtod(p, &end);
        if (end == p)
            break;
        out[n++] = d;
        if (*end != ';')
            break;
        p = end + 1;
    }
    return n;
}

static bool parse_hms(const char *p, int *h, int *m, int *s, const char **end)
{
    if (strlen(p) < 8 || p[2] != ':' || p[5] != ':')
        return false;
    for (int i = 0; i < 8; i++)
        if (i != 2 && i != 5 && (p[i] < '0' || p[i] > '9'))
            return false;
    *h = (p[0] - '0') * 10 + (p[1] - '0');
    *m = (p[3] - '0') * 10 + (p[4] - '0');
    *s = (p[6] - '0') * 10 + (p[7] - '0');
    *end = p + 8;
    return true;
}

static void apply_value(rbx_pilot_state_t *st, char tag, const char *v)
{
    double d[3];
    switch (tag) {
    case RBX_TAG_CLOCK: {
        int h, m, s, ch, cm, cs;
        const char *p;
        if (!parse_hms(v, &h, &m, &s, &p) || *p != ';')
            return;
        p++;
        int sign = 1;
        if (*p == '-') {
            sign = -1;
            p++;
        }
        if (!parse_hms(p, &ch, &cm, &cs, &p))
            return;
        st->clock_h = (uint8_t)h;
        st->clock_m = (uint8_t)m;
        st->clock_s = (uint8_t)s;
        st->chrono_s = sign * (ch * 3600 + cm * 60 + cs);
        break;
    }
    case RBX_TAG_KM_SPEED:
        if (parse_doubles(v, d, 2) == 2) {
            st->km = d[0];
            st->speed_kmh = d[1];
        }
        break;
    case RBX_TAG_REGRESSIVES:
        if (parse_doubles(v, d, 2) == 2) {
            st->km_to_substage_end = d[0];
            st->km_to_next_ref = d[1];
        }
        break;
    case RBX_TAG_TIME_TO_IDEAL:
        if (parse_doubles(v, d, 1) == 1)
            st->time_to_ideal_s = d[0];
        break;
    case RBX_TAG_REFERENCE:
        st->reference = (*v) ? (int32_t)strtol(v, NULL, 10) : -1;
        break;
    case RBX_TAG_TRIP1:
    case RBX_TAG_TRIP2:
        if (parse_doubles(v, d, 3) == 3) {
            int i = tag - RBX_TAG_TRIP1;
            st->trip_km[i] = d[0];
            st->trip_speed_kmh[i] = d[1];
            st->trip_average_kmh[i] = d[2];
        }
        break;
    default: /* '4' and '6' are text only; '3' is unused */
        break;
    }
}

uint16_t rbx_pilot_apply(rbx_pilot_state_t *st, const char *record, size_t len)
{
    if (len < 1 || record[0] < '0' || record[0] > '9' || record[0] == '3')
        return 0;
    char tag = record[0];
    /* The official pilot ignores records shorter than 2 chars; for us an empty '7' means "no reference". */
    if (len < 2 && tag != RBX_TAG_REFERENCE)
        return 0;
    char value[RBX_PILOT_TEXT_MAX];
    size_t vlen = len - 1;
    if (vlen >= sizeof(value))
        vlen = sizeof(value) - 1;
    memcpy(value, record + 1, vlen);
    value[vlen] = '\0';

    char *slot = st->raw[tag - '0'];
    uint16_t bit = RBX_TAG_BIT(tag);
    bool changed = !(st->present & bit) || strcmp(slot, value) != 0;
    st->present |= bit;
    if (!changed)
        return 0;
    strcpy(slot, value);
    apply_value(st, tag, value);
    return bit;
}

/* ------------------------------------------------------------------ encoders */

static int finish(int n, size_t size)
{
    return (n < 0 || (size_t)n >= size) ? -1 : n;
}

int rbx_fmt_tag_clock(char *buf, size_t size, int h, int m, int s, int32_t chrono_s)
{
    long a = labs((long)chrono_s);
    /* hours component of a TimeSpan: days are dropped */
    return finish(snprintf(buf, size, "0%02d:%02d:%02d;%s%02ld:%02ld:%02ld|", h, m, s, chrono_s < 0 ? "-" : "",
                           (a / 3600) % 24, (a / 60) % 60, a % 60),
                  size);
}

int rbx_fmt_tag_km_speed(char *buf, size_t size, double km, double speed_kmh, bool work10m)
{
    return finish(snprintf(buf, size, "1%.*f;%.*f|", work10m ? 2 : 3, km, speed_kmh < 100 ? 1 : 0, speed_kmh), size);
}

int rbx_fmt_tag_regressives(char *buf, size_t size, double to_substage_end, double to_next_ref, bool work10m)
{
    int d = work10m ? 2 : 3;
    return finish(snprintf(buf, size, "2%.*f;%.*f|", d, to_substage_end, d, to_next_ref), size);
}

int rbx_fmt_tag_text(char *buf, size_t size, char tag, const char *text)
{
    if (tag != RBX_TAG_TABLAS && tag != RBX_TAG_SUBSTAGE)
        return -1;
    return finish(snprintf(buf, size, "%c%s|", tag, text ? text : ""), size);
}

int rbx_fmt_tag_time_to_ideal(char *buf, size_t size, double seconds)
{
    return finish(snprintf(buf, size, "5%.*f|", fabs(seconds) < 2 ? 2 : 1, seconds), size);
}

int rbx_fmt_tag_reference(char *buf, size_t size, int32_t reference)
{
    if (reference < 0)
        return finish(snprintf(buf, size, "7|"), size);
    return finish(snprintf(buf, size, "7%ld|", (long)reference), size);
}

int rbx_fmt_tag_trip(char *buf, size_t size, int trip, double km, double speed_kmh, double average_kmh, bool work10m)
{
    if (trip != 1 && trip != 2)
        return -1;
    return finish(snprintf(buf, size, "%c%.*f;%.0f;%.0f|", trip == 1 ? '8' : '9', work10m ? 2 : 3, km, speed_kmh,
                           average_kmh),
                  size);
}
