// dial-racer host tests: the tilt, the messages, the QR texts.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../main/racer.h"

#define RAD (3.14159265f / 180.0f)
static int near(float a, float b, float e) { return fabsf(a - b) < e; }

static const float UPRIGHT[3] = {0, 1, 0};  // held up like a wheel, top edge up
static const float FLAT[3] = {0, 0, -1};    // glass up

static void wheel_turn(float deg, float g[3]) { g[0] = sinf(deg * RAD), g[1] = cosf(deg * RAD), g[2] = 0; }
static void lean_away_upright(float deg, float g[3]) { g[0] = 0, g[1] = cosf(deg * RAD), g[2] = -sinf(deg * RAD); }
static void flat_tilt_right(float deg, float g[3]) { g[0] = sinf(deg * RAD), g[1] = 0, g[2] = -cosf(deg * RAD); }
static void flat_top_down(float deg, float g[3]) { g[0] = 0, g[1] = -sinf(deg * RAD), g[2] = -cosf(deg * RAD); }

static void test_angles(void) {
    racer_tilt_t t;
    racer_tilt_init(&t);
    float g[3], s, p;
    assert(!racer_tilt_angles(&t, UPRIGHT, &s, &p));
    racer_tilt_set_neutral(&t, UPRIGHT);
    wheel_turn(20, g);
    assert(racer_tilt_angles(&t, g, &s, &p) && near(s, 20, 0.1f) && near(p, 0, 0.1f));
    wheel_turn(-30, g);
    racer_tilt_angles(&t, g, &s, &p);
    assert(near(s, -30, 0.1f) && near(p, 0, 0.1f));
    lean_away_upright(15, g);
    racer_tilt_angles(&t, g, &s, &p);
    assert(near(s, 0, 0.1f) && near(p, 15, 0.1f));
    lean_away_upright(-10, g);
    racer_tilt_angles(&t, g, &s, &p);
    assert(near(p, -10, 0.1f));

    racer_tilt_set_neutral(&t, FLAT);
    flat_tilt_right(25, g);
    racer_tilt_angles(&t, g, &s, &p);
    assert(near(s, 25, 0.1f) && near(p, 0, 0.1f));
    flat_top_down(12, g);
    racer_tilt_angles(&t, g, &s, &p);
    assert(near(s, 0, 0.1f) && near(p, 12, 0.1f));
}

static void test_shape(void) {
    assert(racer_shape(2, 3, 40) == 0 && racer_shape(-3, 3, 40) == 0);
    assert(racer_shape(40, 3, 40) == 1 && racer_shape(-90, 3, 40) == -1);
    const float half = racer_shape(21.5f, 3, 40);
    assert(half > 0.3f && half < 0.5f && near(racer_shape(-21.5f, 3, 40), -half, 1e-6f));
    assert(racer_shape(10, 3, 40) < racer_shape(11, 3, 40));
}

static void test_update(void) {
    racer_tilt_t t;
    racer_tilt_init(&t);
    float g[3];
    wheel_turn(30, g);
    racer_tilt_update(&t, g, 0.02f);
    assert(t.steer == 0 && t.push == 0);
    racer_tilt_set_neutral(&t, UPRIGHT);
    racer_tilt_update(&t, g, 0.02f);
    const float target = racer_shape(30, RACER_STEER_DEAD_DEG, RACER_STEER_FULL_DEG);
    assert(t.steer > 0 && t.steer < target);
    for (int i = 0; i < 50; i++) racer_tilt_update(&t, g, 0.02f);
    assert(near(t.steer, target, 0.01f) && near(t.push, 0, 0.01f));
    racer_tilt_set_neutral(&t, g);
    assert(t.steer == 0);
}

static void test_texts(void) {
    char b[96];
    assert(racer_input_json(b, sizeof(b), 0.5f, -2, true));
    assert(!strcmp(b, "{\"t\":\"in\",\"s\":0.500,\"p\":-1.000,\"hb\":1}"));
    assert(racer_input_json(b, 10, 0, 0, false) == 0);
    assert(racer_page_url(b, sizeof(b), "192.168.4.1", "abc123"));
    assert(!strcmp(b, "http://192.168.4.1/#abc123"));
    assert(racer_wifi_qr(b, sizeof(b), "Tikpal Racer", "ke;y:w\\"));
    assert(!strcmp(b, "WIFI:T:WPA;S:Tikpal Racer;P:ke\\;y\\:w\\\\;;"));
    assert(racer_wifi_qr(b, 12, "Tikpal Racer", "abcdefgh") == 0 && b[0] == 0);
    unsigned r[8] = {0, 1, 2, 31, 32, 100, 7, 8};
    char key[9];
    racer_wifi_key(key, 8, r);
    assert(strlen(key) == 8 && !strpbrk(key, "0o1l"));
}

static void test_tele(void) {
    racer_tele_t t;
    assert(racer_parse_tele("{\"t\":\"tele\",\"kmh\":87,\"drift\":1,\"off\":0}", &t));
    assert(t.kmh == 87 && t.drift && !t.off_road);
    assert(racer_parse_tele("{\"t\":\"tele\",\"kmh\":5000}", &t) && t.kmh == 999 && !t.drift);
    assert(racer_parse_tele("{\"t\":\"tele\",\"kmh\":40,\"drift\":0,\"off\":1,\"hit\":3,\"coin\":12,\"fork\":2}", &t));
    assert(t.kmh == 40 && t.off_road && t.hit == 3 && t.coin == 12 && t.fork == 2);
    assert(racer_parse_tele("{\"t\":\"tele\",\"kmh\":1,\"hit\":-4}", &t) && t.hit == 0 && t.coin == 0);
    assert(!racer_parse_tele("{\"t\":\"hello\"}", &t));
    assert(!racer_parse_tele("{\"t\":\"tele\"}", &t));
}

int main(void) {
    test_angles();
    test_shape();
    test_update();
    test_texts();
    test_tele();
    puts("dial-racer host tests: all passed");
    return 0;
}
