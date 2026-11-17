#include "racer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEG (180.0f / 3.14159265f)

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void racer_tilt_init(racer_tilt_t *t) { memset(t, 0, sizeof(*t)); }

void racer_tilt_set_neutral(racer_tilt_t *t, const float g[3]) {
    memcpy(t->neutral, g, sizeof(t->neutral));
    t->has_neutral = true;
    t->steer = t->push = 0;
}

bool racer_tilt_angles(const racer_tilt_t *t, const float g[3], float *steer_deg, float *push_deg) {
    if (!t->has_neutral) return false;
    const float *n = t->neutral;
    *steer_deg = (asinf(clampf(g[0], -1, 1)) - asinf(clampf(n[0], -1, 1))) * DEG;
    // The rotation from the neutral to now in the y-z plane, about x; leaning the top away is negative.
    const float cross = n[1] * g[2] - n[2] * g[1], dot = n[1] * g[1] + n[2] * g[2];
    *push_deg = -atan2f(cross, dot) * DEG;
    return true;
}

float racer_shape(float deg, float dead, float full) {
    const float a = fabsf(deg);
    if (a <= dead) return 0;
    const float v = powf(clampf((a - dead) / (full - dead), 0, 1), 1.2f);
    return deg < 0 ? -v : v;
}

void racer_tilt_update(racer_tilt_t *t, const float g[3], float dt) {
    float sd, pd;
    if (!racer_tilt_angles(t, g, &sd, &pd)) {
        t->steer = t->push = 0;
        return;
    }
    const float s = racer_shape(sd, RACER_STEER_DEAD_DEG, RACER_STEER_FULL_DEG);
    const float p = racer_shape(pd, RACER_PUSH_DEAD_DEG, RACER_PUSH_FULL_DEG);
    const float k = dt <= 0 ? 1 : dt / (RACER_SMOOTH_S + dt);
    t->steer += (s - t->steer) * k;
    t->push += (p - t->push) * k;
}

size_t racer_input_json(char *out, size_t cap, float steer, float push, bool hb) {
    const int n = snprintf(out, cap, "{\"t\":\"in\",\"s\":%.3f,\"p\":%.3f,\"hb\":%d}", (double)clampf(steer, -1, 1),
                           (double)clampf(push, -1, 1), hb ? 1 : 0);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

size_t racer_page_url(char *out, size_t cap, const char *ip, const char *token) {
    const int n = snprintf(out, cap, "http://%s/#%s", ip, token);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

static bool put(char *out, size_t cap, size_t *at, const char *s, bool escape) {
    for (; *s; s++) {
        if (escape && strchr("\\;,:\"", *s)) {
            if (*at + 1 >= cap) return false;
            out[(*at)++] = '\\';
        }
        if (*at + 1 >= cap) return false;
        out[(*at)++] = *s;
    }
    return true;
}

size_t racer_wifi_qr(char *out, size_t cap, const char *ssid, const char *key) {
    size_t at = 0;
    if (!cap) return 0;
    if (!put(out, cap, &at, "WIFI:T:WPA;S:", false) || !put(out, cap, &at, ssid, true) ||
        !put(out, cap, &at, ";P:", false) || !put(out, cap, &at, key, true) || !put(out, cap, &at, ";;", false)) {
        out[0] = 0;
        return 0;
    }
    out[at] = 0;
    return at;
}

void racer_wifi_key(char *out, size_t len, const unsigned *random) {
    static const char set[] = "abcdefghijkmnpqrstuvwxyz23456789";
    for (size_t i = 0; i < len; i++) out[i] = set[random[i] % (sizeof(set) - 1)];
    out[len] = 0;
}

static bool num_after(const char *text, const char *key, double *v) {
    const char *p = strstr(text, key);
    if (!p) return false;
    p += strlen(key);
    char *end;
    *v = strtod(p, &end);
    return end != p;
}

bool racer_parse_tele(const char *text, racer_tele_t *out) {
    if (!strstr(text, "\"t\":\"tele\"")) return false;
    double kmh = 0, drift = 0, off = 0, hit = 0, coin = 0, fork = 0;
    if (!num_after(text, "\"kmh\":", &kmh)) return false;
    num_after(text, "\"drift\":", &drift);
    num_after(text, "\"off\":", &off);
    num_after(text, "\"hit\":", &hit);
    num_after(text, "\"coin\":", &coin);
    num_after(text, "\"fork\":", &fork);
    out->hit = (int)clampf((float)hit, 0, 1e6f);
    out->coin = (int)clampf((float)coin, 0, 1e6f);
    out->fork = (int)clampf((float)fork, 0, 1e6f);
    out->kmh = (int)clampf((float)kmh, 0, 999);
    out->drift = drift != 0;
    out->off_road = off != 0;
    return true;
}
