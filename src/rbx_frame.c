/**
 * @file rbx_frame.c
 * @brief Record splitter and box-stream encoders/parser. Portable C99, no ESP-IDF.
 */
#include "rbx_frame.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FIELDS 8

/* ------------------------------------------------------------------ splitter */

void rbx_splitter_init(rbx_splitter_t *s)
{
    s->len = 0;
    s->discard = false;
}

static bool is_blank(const char *p, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (p[i] != ' ' && p[i] != '\t' && p[i] != '\0')
            return false;
    return true;
}

void rbx_splitter_feed(rbx_splitter_t *s, const char *data, size_t len, rbx_record_cb_t cb, void *ctx)
{
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\r' || c == '\n')
            continue;
        if (c == '|') {
            if (!s->discard && !is_blank(s->buf, s->len)) {
                s->buf[s->len] = '\0';
                cb(s->buf, s->len, ctx);
            }
            s->len = 0;
            s->discard = false;
            continue;
        }
        if (s->discard)
            continue;
        if (s->len >= RBX_RECORD_MAX - 1) {
            s->discard = true;
            continue;
        }
        s->buf[s->len++] = c;
    }
}

/* ------------------------------------------------------------------ helpers */

/** Split `text` (modified in place) on `sep`; returns the field count, or -1 if more than max. */
static int split(char *text, char sep, char **fields, int max)
{
    int n = 0;
    char *p = text;
    while (1) {
        if (n == max)
            return -1;
        fields[n++] = p;
        char *q = strchr(p, sep);
        if (!q)
            return n;
        *q = '\0';
        p = q + 1;
    }
}

static bool to_i64(const char *s, int64_t *out)
{
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (end == s || *end != '\0' || errno)
        return false;
    *out = v;
    return true;
}

static bool to_u64(const char *s, uint64_t *out)
{
    char *end;
    if (*s == '-')
        return false;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s || *end != '\0' || errno)
        return false;
    *out = v;
    return true;
}

static bool to_double(const char *s, double *out)
{
    char *end;
    double v = strtod(s, &end);
    if (end == s || *end != '\0')
        return false;
    *out = v;
    return true;
}

static int finish(int n, size_t size)
{
    return (n < 0 || (size_t)n >= size) ? -1 : n;
}

/* ------------------------------------------------------------------ box stream */

static const char *const MSG_KEYWORDS[] = {
    [RBX_MSG_PULSE] = "pulse",      [RBX_MSG_PULSE_COORDS] = "pc",  [RBX_MSG_PULSE_ENCRYPTED] = "pe",
    [RBX_MSG_BT_UP] = "btUp",       [RBX_MSG_BT_DOWN] = "btDown",   [RBX_MSG_BT_UP_10] = "btUp10",
    [RBX_MSG_BT_DOWN_10] = "btDown10", [RBX_MSG_LAP] = "lap",       [RBX_MSG_CKM] = "ckm",
};

static const char *const GPS_KEYWORDS[] = {
    [RBX_GPS_PG] = "pg", [RBX_GPS_PG1] = "pg1", [RBX_GPS_PG2] = "pg2", [RBX_GPS_CO] = "co", [RBX_GPS_CI] = "ci",
};

const char *rbx_msg_keyword(rbx_msg_type_t type)
{
    if (type <= RBX_MSG_INVALID || type > RBX_MSG_CKM || type == RBX_MSG_GPS)
        return NULL;
    return MSG_KEYWORDS[type];
}

const char *rbx_gps_keyword(rbx_gps_kind_t kind)
{
    return (kind <= RBX_GPS_CI) ? GPS_KEYWORDS[kind] : NULL;
}

static bool parse_pulse(char *body, rbx_box_msg_t *out)
{
    char *f[MAX_FIELDS];
    int n = split(body, ';', f, MAX_FIELDS);
    out->type = RBX_MSG_PULSE;
    if (n == 3)
        return to_i64(f[0], &out->s1) && to_u64(f[1], &out->ms) && ((out->s2 = out->s1), true);
    if (n == 4)
        return to_i64(f[0], &out->s1) && to_i64(f[1], &out->s2) && to_u64(f[2], &out->ms);
    return false;
}

static bool parse_digits(const char *p, int count, int *out)
{
    int v = 0;
    for (int i = 0; i < count; i++) {
        if (p[i] < '0' || p[i] > '9')
            return false;
        v = v * 10 + (p[i] - '0');
    }
    *out = v;
    return true;
}

static bool parse_gps(char *body, rbx_gps_fix_t *fix)
{
    char *f[MAX_FIELDS];
    int n = split(body, '\t', f, MAX_FIELDS);
    if (n != 6 && n != 7)
        return false;
    /* yyyyMMddHHmmss[f] */
    int y, mo, d, h, mi, s, t = 0;
    const char *ts = f[0];
    size_t tl = strlen(ts);
    if (tl < 14 || !parse_digits(ts, 4, &y) || !parse_digits(ts + 4, 2, &mo) || !parse_digits(ts + 6, 2, &d) ||
        !parse_digits(ts + 8, 2, &h) || !parse_digits(ts + 10, 2, &mi) || !parse_digits(ts + 12, 2, &s) ||
        (tl > 14 && !parse_digits(ts + 14, 1, &t)))
        return false;
    fix->year = (uint16_t)y;
    fix->month = (uint8_t)mo;
    fix->day = (uint8_t)d;
    fix->hour = (uint8_t)h;
    fix->minute = (uint8_t)mi;
    fix->second = (uint8_t)s;
    fix->tenth = (uint8_t)t;
    int64_t speed, heading;
    double acc, alt = 0;
    if (!to_double(f[1], &fix->lat) || !to_double(f[2], &fix->lon) || !to_i64(f[3], &speed) ||
        !to_i64(f[4], &heading) || !to_double(f[5], &acc))
        return false;
    fix->speed_kmh = (int32_t)speed;
    fix->heading_deg = (int32_t)heading;
    fix->accuracy = (float)acc;
    fix->has_altitude = (n == 7) && to_double(f[6], &alt);
    fix->altitude = (float)alt;
    return true;
}

static bool parse_button(char *body, rbx_box_msg_t *out)
{
    char *f[MAX_FIELDS];
    int n = split(body, ';', f, MAX_FIELDS);
    if (n < 1 || !to_i64(f[0], &out->s1))
        return false;
    out->s2 = out->s1;
    if (n > 1 && !to_i64(f[1], &out->s2))
        return false;
    return true;
}

bool rbx_parse_box_record(const char *record, size_t len, rbx_box_msg_t *out)
{
    char text[RBX_RECORD_MAX];
    memset(out, 0, sizeof(*out));
    out->type = RBX_MSG_INVALID;

    /* Trim, keeping embedded NULs out of the way of string functions. */
    while (len && (*record == ' ' || *record == '\t'))
        record++, len--;
    while (len && (record[len - 1] == ' ' || record[len - 1] == '\t'))
        len--;
    if (len == 0 || len >= sizeof(text))
        return false;

    /* The app accepts the last "pulse=" anywhere in the record, and repairs "\0ulse=". */
    for (size_t k = (len >= 6) ? len - 5 : 0; k-- > 0;) {
        if ((record[k] == 'p' || record[k] == '\0') && memcmp(record + k + 1, "ulse=", 5) == 0) {
            size_t start = k + 6;
            memcpy(text, record + start, len - start);
            text[len - start] = '\0';
            if (!parse_pulse(text, out)) {
                out->type = RBX_MSG_INVALID;
                return false;
            }
            return true;
        }
    }

    memcpy(text, record, len);
    text[len] = '\0';
    if (memchr(text, '\0', len))
        return false;
    char *eq = strchr(text, '=');
    if (!eq)
        return false;
    *eq = '\0';
    const char *kw = text;
    char *body = eq + 1;

    if (strcmp(kw, "pc") == 0) {
        char *f[MAX_FIELDS];
        out->type = RBX_MSG_PULSE_COORDS;
        if (split(body, ';', f, MAX_FIELDS) == 4 && to_i64(f[0], &out->s1) && to_u64(f[1], &out->ms) &&
            to_double(f[2], &out->lon) && to_double(f[3], &out->lat)) {
            out->s2 = out->s1;
            return true;
        }
    }
    else if (strcmp(kw, "pe") == 0) {
        out->type = RBX_MSG_PULSE_ENCRYPTED;
        return true;
    }
    else {
        for (int k = RBX_GPS_PG; k <= RBX_GPS_CI; k++) {
            if (strcmp(kw, GPS_KEYWORDS[k]) == 0) {
                out->type = RBX_MSG_GPS;
                out->gps_kind = (rbx_gps_kind_t)k;
                if (parse_gps(body, &out->fix))
                    return true;
                out->type = RBX_MSG_INVALID;
                return false;
            }
        }
        for (int t = RBX_MSG_BT_UP; t <= RBX_MSG_CKM; t++) {
            if (strcmp(kw, MSG_KEYWORDS[t]) == 0) {
                out->type = (rbx_msg_type_t)t;
                if (parse_button(body, out))
                    return true;
                break;
            }
        }
    }
    out->type = RBX_MSG_INVALID;
    return false;
}

int rbx_fmt_pulse(char *buf, size_t size, int64_t s1, int64_t s2, uint64_t ms)
{
    return finish(snprintf(buf, size, "pulse=%lld;%lld;%llu;00|\n", (long long)s1, (long long)s2,
                           (unsigned long long)ms), size);
}

int rbx_fmt_pulse_coords(char *buf, size_t size, int64_t count, uint64_t ms, double lat, double lon)
{
    /* Longitude before latitude, as the app expects. */
    return finish(snprintf(buf, size, "pc=%lld;%llu;%.7f;%.7f|\n", (long long)count, (unsigned long long)ms, lon, lat),
                  size);
}

int rbx_fmt_button(char *buf, size_t size, rbx_msg_type_t type, int64_t s1, int64_t s2)
{
    if (type < RBX_MSG_BT_UP || type > RBX_MSG_CKM)
        return -1;
    return finish(snprintf(buf, size, "%s=%lld;%lld|\n", MSG_KEYWORDS[type], (long long)s1, (long long)s2), size);
}

int rbx_fmt_gps(char *buf, size_t size, rbx_gps_kind_t kind, const rbx_gps_fix_t *f)
{
    if (kind > RBX_GPS_CI)
        return -1;
    return finish(snprintf(buf, size, "%s=%04u%02u%02u%02u%02u%02u%u\t%.7f\t%.7f\t%ld\t%ld\t%.1f\t%.0f|\n",
                           GPS_KEYWORDS[kind], f->year, f->month, f->day, f->hour, f->minute, f->second,
                           f->tenth % 10, f->lat, f->lon, (long)f->speed_kmh, (long)f->heading_deg,
                           (double)f->accuracy, (double)f->altitude),
                  size);
}
