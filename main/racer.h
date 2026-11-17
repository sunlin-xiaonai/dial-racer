// Dial Racer's logic without ESP-IDF (host tested in test/): the round screen held like a steering wheel.
//
// Gravity comes in screen coordinates (yaykit_motion: x right, y down, z out of the glass, unit length). The neutral
// is the gravity when the player pressed start. Two numbers come out, both -1..1:
//   steer  turning the wheel (or tilting the right edge down when held flat): + is right
//   push   leaning the top edge away from you: + is gas, - is brake
// The two are measured on different axes (x against the y-z plane), so turning does not change the gas and back.
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    float neutral[3];
    bool has_neutral;
    float steer, push;  // smoothed, -1..1
} racer_tilt_t;

#define RACER_STEER_DEAD_DEG 3.0f
#define RACER_STEER_FULL_DEG 40.0f
#define RACER_PUSH_DEAD_DEG 5.0f
#define RACER_PUSH_FULL_DEG 25.0f
#define RACER_SMOOTH_S 0.05f

void racer_tilt_init(racer_tilt_t *t);
/** The way it is held now becomes straight ahead, standing still. */
void racer_tilt_set_neutral(racer_tilt_t *t, const float gravity[3]);
/** Raw angles from the neutral, degrees: steer + right, push + gas. False without a neutral. */
bool racer_tilt_angles(const racer_tilt_t *t, const float gravity[3], float *steer_deg, float *push_deg);
/** A new gravity sample `dt_s` after the last: updates t->steer and t->push (0 without a neutral). */
void racer_tilt_update(racer_tilt_t *t, const float gravity[3], float dt_s);
/** degrees -> -1..1 with a dead zone and a gentle curve. */
float racer_shape(float deg, float dead_deg, float full_deg);

/** The message to the iPad: {"t":"in","s":0.123,"p":-0.456,"hb":0}. Returns the length, 0 if it did not fit. */
size_t racer_input_json(char *out, size_t cap, float steer, float push, bool handbrake);
/** The iPad's page with the token: http://<ip>/#<token>. 0 if it did not fit. */
size_t racer_page_url(char *out, size_t cap, const char *ip, const char *token);
/** A Wi-Fi QR code's text (WIFI:T:WPA;S:<ssid>;P:<key>;;), with \ ; , : " escaped. 0 if it did not fit. */
size_t racer_wifi_qr(char *out, size_t cap, const char *ssid, const char *key);
/** A hotspot key from `len` random numbers: letters and digits easy to read off the screen (no 0 o 1 l). */
void racer_wifi_key(char *out, size_t len, const unsigned *random);

typedef struct {
    int kmh;
    bool drift, off_road;
    int hit, coin, fork;  // counters since the page opened: crashes, coins, forks taken
} racer_tele_t;

/** The iPad's {"t":"tele","kmh":87,"drift":1,"off":0,"hit":2,"coin":9,"fork":1}: true and `out` filled when it is one. */
bool racer_parse_tele(const char *text, racer_tele_t *out);
