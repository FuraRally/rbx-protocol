/* Host tests for rbx_frame.c / rbx_pilot_state.c. Exit code = number of failures. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "rbx_frame.h"

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                            \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)
#define CHECK_STR(a, b) CHECK(strcmp((a), (b)) == 0)
#define CHECK_NEAR(a, b) CHECK(fabs((double)(a) - (double)(b)) < 1e-9)

/* --- splitter */
static char got[8][RBX_RECORD_MAX];
static int ngot;
static void collect(const char *rec, size_t len, void *ctx)
{
    (void)ctx;
    if (ngot < 8)
        memcpy(got[ngot++], rec, len + 1);
}

static void test_splitter(void)
{
    rbx_splitter_t s;
    rbx_splitter_init(&s);
    ngot = 0;
    const char *a = "pulse=1;2;3;00|\npul";
    const char *b = "se=4;5;6;00|btUp=1|\r\n  |";
    rbx_splitter_feed(&s, a, strlen(a), collect, NULL);
    rbx_splitter_feed(&s, b, strlen(b), collect, NULL);
    CHECK(ngot == 3);
    CHECK_STR(got[0], "pulse=1;2;3;00");
    CHECK_STR(got[1], "pulse=4;5;6;00");
    CHECK_STR(got[2], "btUp=1");

    char big[RBX_RECORD_MAX * 2];
    memset(big, 'x', sizeof(big));
    ngot = 0;
    rbx_splitter_feed(&s, big, sizeof(big), collect, NULL);
    rbx_splitter_feed(&s, "|lap=7|", 7, collect, NULL);
    CHECK(ngot == 1); /* the oversized record is dropped */
    CHECK_STR(got[0], "lap=7");
}

/* --- box records */
static bool parse(const char *s, rbx_box_msg_t *m)
{
    return rbx_parse_box_record(s, strlen(s), m);
}

static void test_box_parse(void)
{
    rbx_box_msg_t m;
    CHECK(parse("pulse=3333;3607;331560;00", &m) && m.type == RBX_MSG_PULSE && m.s1 == 3333 && m.s2 == 3607 &&
          m.ms == 331560);
    CHECK(parse("pulse=10;300;00", &m) && m.s1 == 10 && m.s2 == 10 && m.ms == 300); /* 3 fields */
    CHECK(rbx_parse_box_record("\0ulse=1;2;3;00", 14, &m) && m.type == RBX_MSG_PULSE && m.s2 == 2);
    CHECK(parse("garbagepulse=5;6;7;00", &m) && m.s1 == 5);
    CHECK(!parse("pulse=1;2", &m));
    CHECK(!parse("pulse=a;2;3;00", &m));
    CHECK(parse("pc=5;100;2.17;41.38", &m) && m.type == RBX_MSG_PULSE_COORDS && m.lon == 2.17 && m.lat == 41.38);
    CHECK(parse("pe=12.34.56", &m) && m.type == RBX_MSG_PULSE_ENCRYPTED);
    CHECK(parse("lap=1937", &m) && m.type == RBX_MSG_LAP && m.s1 == 1937 && m.s2 == 1937);
    CHECK(parse("btDown10=2161;2307", &m) && m.type == RBX_MSG_BT_DOWN_10 && m.s2 == 2307);
    CHECK(parse("ckm=1943;2066", &m) && m.type == RBX_MSG_CKM);
    CHECK(!parse("hello", &m));
    CHECK(!parse("btUp=", &m));

    CHECK(parse("co=202610301200013\t41.1234567\t2.7654321\t48\t90\t1.5\t120", &m) && m.type == RBX_MSG_GPS &&
          m.gps_kind == RBX_GPS_CO && m.fix.year == 2026 && m.fix.second == 1 && m.fix.tenth == 3 &&
          m.fix.speed_kmh == 48 && m.fix.has_altitude && m.fix.lat == 41.1234567);
    CHECK(parse("pg1=20261030120001\t41\t2\t10\t0\t3.0", &m) && m.gps_kind == RBX_GPS_PG1 && !m.fix.has_altitude);
    CHECK(!parse("pg=20261030120001\t41\t2\t10", &m)); /* too few fields */
}

static void test_box_fmt(void)
{
    char buf[128];
    CHECK(rbx_fmt_pulse(buf, sizeof(buf), 15, 16, 15888) > 0);
    CHECK_STR(buf, "pulse=15;16;15888;00|\n");
    CHECK(rbx_fmt_button(buf, sizeof(buf), RBX_MSG_BT_UP_10, 20, 21) > 0);
    CHECK_STR(buf, "btUp10=20;21|\n");
    CHECK(rbx_fmt_button(buf, sizeof(buf), RBX_MSG_PULSE, 1, 1) == -1);
    CHECK(rbx_fmt_pulse_coords(buf, sizeof(buf), 5, 100, 41.38, 2.17) > 0);
    CHECK_STR(buf, "pc=5;100;2.1700000;41.3800000|\n");
    rbx_gps_fix_t f = { 2026, 10, 30, 12, 0, 1, 3, 41.1234567, 2.7654321, 48, 90, 1.5f, 120, true };
    CHECK(rbx_fmt_gps(buf, sizeof(buf), RBX_GPS_CO, &f) > 0);
    CHECK_STR(buf, "co=202610301200013\t41.1234567\t2.7654321\t48\t90\t1.5\t120|\n");

    /* round trip */
    rbx_box_msg_t m;
    buf[strlen(buf) - 2] = '\0';
    CHECK(parse(buf, &m) && m.fix.lon == 2.7654321 && m.fix.heading_deg == 90);

    CHECK(rbx_fmt_pulse(buf, 8, 1, 2, 3) == -1); /* too small */
}

/* --- pilot */
static uint16_t apply(rbx_pilot_state_t *st, const char *rec)
{
    return rbx_pilot_apply(st, rec, strlen(rec));
}

static void feed_datagram(rbx_pilot_state_t *st, const char *dgram, uint16_t *mask)
{
    char tmp[256];
    strcpy(tmp, dgram);
    char *p = tmp;
    char *bar;
    while ((bar = strchr(p, '|'))) {
        *bar = '\0';
        *mask |= rbx_pilot_apply(st, p, strlen(p));
        p = bar + 1;
    }
}

static void test_pilot_apply(void)
{
    rbx_pilot_state_t st;
    rbx_pilot_state_init(&st);
    uint16_t mask = 0;
    /* Real datagrams, as captured from the Rabbit 2 app. */
    feed_datagram(&st, "10.653;30.0|572248.6|6E76>NN|73|25.656;1.162|013:35:39;20:04:39|", &mask);
    CHECK(mask == (RBX_TAG_BIT('0') | RBX_TAG_BIT('1') | RBX_TAG_BIT('2') | RBX_TAG_BIT('5') | RBX_TAG_BIT('6') |
                   RBX_TAG_BIT('7')));
    CHECK_NEAR(st.km, 0.653);
    CHECK_NEAR(st.speed_kmh, 30.0);
    CHECK_NEAR(st.time_to_ideal_s, 72248.6);
    CHECK_STR(rbx_pilot_text(&st, '6'), "E76>NN");
    CHECK(st.reference == 3);
    CHECK_NEAR(st.km_to_substage_end, 5.656);
    CHECK(st.clock_h == 13 && st.clock_m == 35 && st.chrono_s == 20 * 3600 + 4 * 60 + 39);

    mask = 0;
    feed_datagram(&st, "10.653;30.0|5-1.25|6V49,99>V30|7|013:37:13;-00:00:16|", &mask);
    CHECK(!(mask & RBX_TAG_BIT('1'))); /* unchanged */
    CHECK_NEAR(st.time_to_ideal_s, -1.25);
    CHECK_STR(rbx_pilot_text(&st, '6'), "V49,99>V30");
    CHECK(st.reference == -1);
    CHECK(st.chrono_s == -16);

    CHECK(apply(&st, "400:31.7") == RBX_TAG_BIT('4'));
    CHECK_STR(rbx_pilot_text(&st, '4'), "00:31.7");
    CHECK(apply(&st, "81.234;48;50") == RBX_TAG_BIT('8'));
    CHECK_NEAR(st.trip_km[0], 1.234);
    CHECK_NEAR(st.trip_average_kmh[0], 50);
    CHECK(apply(&st, "3x") == 0);  /* tag 3 unused */
    CHECK(apply(&st, "5") == 0);   /* too short */
    CHECK(apply(&st, "A12") == 0); /* not a tag */
    CHECK(apply(&st, "0garbage") == RBX_TAG_BIT('0') && st.chrono_s == -16); /* text kept, numbers untouched */
}

static void test_pilot_fmt(void)
{
    char buf[64];
    rbx_fmt_tag_clock(buf, sizeof(buf), 13, 35, 39, -16);
    CHECK_STR(buf, "013:35:39;-00:00:16|");
    rbx_fmt_tag_clock(buf, sizeof(buf), 0, 0, 0, 86400 + 61);
    CHECK_STR(buf, "000:00:00;00:01:01|"); /* days dropped */
    rbx_fmt_tag_km_speed(buf, sizeof(buf), 1.0365, 49.14, false);
    CHECK_STR(buf, "11.036;49.1|"); /* note: printf rounding (binary), the app rounds decimals half-away */
    rbx_fmt_tag_km_speed(buf, sizeof(buf), 12.5, 123.4, true);
    CHECK_STR(buf, "112.50;123|");
    rbx_fmt_tag_regressives(buf, sizeof(buf), 5.656, -1.655, false);
    CHECK_STR(buf, "25.656;-1.655|");
    rbx_fmt_tag_time_to_ideal(buf, sizeof(buf), 1.234);
    CHECK_STR(buf, "51.23|");
    rbx_fmt_tag_time_to_ideal(buf, sizeof(buf), -12.34);
    CHECK_STR(buf, "5-12.3|");
    rbx_fmt_tag_text(buf, sizeof(buf), '6', "V50>E30");
    CHECK_STR(buf, "6V50>E30|");
    CHECK(rbx_fmt_tag_text(buf, sizeof(buf), '1', "x") == -1);
    rbx_fmt_tag_reference(buf, sizeof(buf), -1);
    CHECK_STR(buf, "7|");
    rbx_fmt_tag_reference(buf, sizeof(buf), 183);
    CHECK_STR(buf, "7183|");
    rbx_fmt_tag_trip(buf, sizeof(buf), 2, 3.2, 48.6, 50.2, false);
    CHECK_STR(buf, "93.200;49;50|");
}

int main(void)
{
    test_splitter();
    test_box_parse();
    test_box_fmt();
    test_pilot_apply();
    test_pilot_fmt();
    printf("%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    return failures;
}
