// Dial Racer: the round screen is the steering wheel of a toy car standing on an iPad. The iPad's page draws the road
// moving under the car; the round screen turns its tilt into steering and gas and sends them over Wi-Fi.
//
//   round screen                          iPad (Safari)
//   yaykit_motion gravity -> racer.c  --> ws://<ip>:8787/racer  {"t":"in","s","p","hb"} 30 times a second
//   http://<ip>/ serves main/web/pad.html  <-- {"t":"tele","kmh","drift","off","hit","coin","fork"} 10 times a second
//
// The network: the shared Wi-Fi when the round screen is on one (the iPad joins the same), else the round screen's
// own hotspot (a Wi-Fi QR code, then the page's). The page's QR carries yaykit_lan's token: only an iPad that
// scanned the screen can drive. Start order as the template: NVS, yaykit_app, board, key, link, screen, console, ready.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "cjk_font.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "racer.h"
#include "yaykit_app.h"
#include "yaykit_key.h"
#include "yaykit_lan.h"
#include "yaykit_link.h"
#include "yaykit_motion.h"
#include "yaykit_ui.h"
#include "yaykit_wifi.h"
#include "yk_hint.h"
#include "yk_wifi_ap.h"

#define APP_SLUG "dial-racer"
#define NVS_NS "yk_dialracer"
#define LAN_PORT 8787
#define LAN_PATH "/racer"
static const char *TAG = "dial-racer";

extern const char pad_html_start[] asm("_binary_pad_html_start");
extern const char pad_html_end[] asm("_binary_pad_html_end");

typedef enum { ST_NET, ST_PAIR, ST_READY, ST_DRIVE } state_t;

static state_t s_state = ST_NET;
static bool s_en;
static volatile bool s_net_up, s_hotspot, s_hotspot_busy, s_hello, s_closed, s_ap_client;
static volatile bool s_want_hotspot, s_want_start, s_want_neutral, s_key_hold, s_touch_hold, s_motion_ok;
static char s_ip[16] = "192.168.4.1", s_token[YK_LAN_TOKEN_LEN + 1], s_ssid[24], s_key[12];
static racer_tilt_t s_tilt;
static int64_t s_last_us;
static volatile racer_tele_t s_tele;
static int s_top_kmh;
static int64_t s_drive_ms, s_drive_from;
static bool s_lost;

static const char *T(const char *zh, const char *en) { return s_en ? en : zh; }

// ---------------------------------------------------------------- tilt (the motion task)

static void on_samples(const yk_motion_sample_t *s, size_t n, void *ctx) {
    (void)ctx;
    if (!n) return;
    const yk_motion_sample_t *last = &s[n - 1];
    if (s_want_neutral) {
        racer_tilt_set_neutral(&s_tilt, last->gravity);
        s_want_neutral = false;
    }
    const float dt = s_last_us ? (float)(last->t_us - s_last_us) / 1e6f : 0.02f;
    s_last_us = last->t_us;
    racer_tilt_update(&s_tilt, last->gravity, dt);
}

// ---------------------------------------------------------------- the iPad (yaykit_lan's server task: flags only)

static void on_hello(const char *name, void *ctx) {
    (void)name, (void)ctx;
    s_hello = true;
}

static void on_text(const char *text, size_t len, void *ctx) {
    (void)len, (void)ctx;
    racer_tele_t t;
    if (racer_parse_tele(text, &t)) s_tele = t;
}

static void on_binary(uint8_t *data, size_t len, void *ctx) {
    (void)len, (void)ctx;
    heap_caps_free(data);
}

static void on_lan_close(void *ctx) {
    (void)ctx;
    s_closed = true;
}

static void on_ap_client(bool joined, void *ctx) {
    (void)ctx;
    s_ap_client = joined;
}

// ---------------------------------------------------------------- the page and the network

static esp_err_t page_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, pad_html_start, pad_html_end - pad_html_start - 1);  // EMBED_TXTFILES adds a NUL
}

static esp_err_t start_web(void) {
    httpd_handle_t server = NULL;
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.server_port = 80;
    c.ctrl_port = LAN_PORT + 10;
    c.max_open_sockets = 3;
    c.lru_purge_enable = true;
    c.stack_size = 4096;
    c.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    esp_err_t err = httpd_start(&server, &c);
    if (err != ESP_OK) return err;
    const httpd_uri_t page = {.uri = "/", .method = HTTP_GET, .handler = page_get};
    return httpd_register_uri_handler(server, &page);
}

/** The hotspot's name and key: made once and kept, so the iPad remembers the network. */
static void load_hotspot(void) {
    nvs_handle_t h;
    size_t a = sizeof(s_ssid), b = sizeof(s_key);
    const bool open = nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK;
    if (open && nvs_get_str(h, "ssid", s_ssid, &a) == ESP_OK && nvs_get_str(h, "key", s_key, &b) == ESP_OK) {
        nvs_close(h);
        return;
    }
    unsigned r[10];
    for (int i = 0; i < 10; i++) r[i] = esp_random();
    char tag[5];
    racer_wifi_key(tag, 4, r);
    snprintf(s_ssid, sizeof(s_ssid), "Tikpal-Racer-%s", tag);
    for (int i = 0; i < 10; i++) r[i] = esp_random();
    racer_wifi_key(s_key, 10, r);
    if (open) {
        nvs_set_str(h, "ssid", s_ssid);
        nvs_set_str(h, "key", s_key);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void start_hotspot(void) {
    load_hotspot();
    yk_wifi_ap_on_client(on_ap_client, NULL);
    const esp_err_t err = yk_wifi_ap_start(&(yk_wifi_ap_config_t){.ssid = s_ssid, .password = s_key, .max_clients = 1});
    if (err != ESP_OK) ESP_LOGE(TAG, "hotspot: %s", esp_err_to_name(err));
    snprintf(s_ip, sizeof(s_ip), "%s", YK_WIFI_AP_IP);
    s_ap_client = yk_wifi_ap_clients() > 0;
    s_hotspot = true;
}

static void hotspot_task(void *arg) {
    (void)arg;
    start_hotspot();
    s_hotspot_busy = false;
    vTaskDelete(NULL);
}

static void net_task(void *arg) {
    (void)arg;
    bool home = false;
    if (yk_wifi_start() == ESP_OK && yk_wifi_state() != YK_WIFI_UNCONFIGURED) home = yk_wifi_wait_connected(8000);
    esp_netif_ip_info_t ip;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (home && sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr) {
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip.ip));
    } else {
        start_hotspot();
    }
    esp_err_t err = start_web();
    if (err != ESP_OK) ESP_LOGE(TAG, "page server: %s", esp_err_to_name(err));
    const yk_lan_config_t lan = {.port = LAN_PORT, .path = LAN_PATH, .max_binary = 1024, .on_hello = on_hello,
                                 .on_text = on_text, .on_binary = on_binary, .on_close = on_lan_close};
    err = yk_lan_start(&lan);
    if (err != ESP_OK) ESP_LOGE(TAG, "lan: %s", esp_err_to_name(err));
    yk_lan_new_token(s_token);
    printf("DIAL_RACER_NET mode=%s\n", s_hotspot ? "hotspot" : "wifi");
    s_net_up = true;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------- the screen (LVGL task)

static lv_obj_t *s_screen, *s_pair, *s_ready, *s_drive;
static lv_obj_t *s_net_label, *s_pair_title, *s_qr, *s_pair_line, *s_pair_d1, *s_pair_d2;
static lv_obj_t *s_speed, *s_status, *s_pad_ring, *s_ball, *s_arrow;
static int s_shown_kmh = -1, s_shown_bx = 999, s_shown_by = 999;
static const char *s_shown_status;
static racer_tele_t s_seen;
static int64_t s_event_until;
static char s_event[40];
static lv_point_precise_t s_arrow_pts[2];

#define PAD_X YK_CX
#define PAD_Y 262
#define PAD_R 108
#define PAD_REACH 82
#define HUB_R 24
#define BALL_R 17
static char s_qr_text[96];

static lv_obj_t *layer(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, 466, 466);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

/** A circle of radius r around (cx, cy): filled with `fill`, or only a `border` wide ring when fill is 0. */
static lv_obj_t *dot(lv_obj_t *parent, int cx, int cy, int r, uint32_t fill, int border, uint32_t border_color) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o, r * 2, r * 2);
    lv_obj_set_pos(o, cx - r, cy - r);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    if (fill) {
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(fill), 0);
    }
    if (border) {
        lv_obj_set_style_border_width(o, border, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(border_color), 0);
    }
    return o;
}

static lv_obj_t *stroke(lv_obj_t *parent, int width, uint32_t color) {
    lv_obj_t *l = lv_line_create(parent);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    return l;
}

/** A small chevron at the pad's edge pointing out: up (0), right (1), down (2), left (3). */
static void chevron(lv_obj_t *parent, int dir, uint32_t color) {
    static lv_point_precise_t pts[4][3];
    static const int dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0};
    const int tip = PAD_R - 14, base = PAD_R - 30, half = 13;
    lv_point_precise_t *p = pts[dir];
    p[0].x = PAD_X + dx[dir] * base - dy[dir] * half, p[0].y = PAD_Y + dy[dir] * base + dx[dir] * half;
    p[1].x = PAD_X + dx[dir] * tip, p[1].y = PAD_Y + dy[dir] * tip;
    p[2].x = PAD_X + dx[dir] * base + dy[dir] * half, p[2].y = PAD_Y + dy[dir] * base - dx[dir] * half;
    lv_line_set_points(stroke(parent, 6, color), p, 3);
}

static void show_only(lv_obj_t *o) {
    lv_obj_t *all[] = {s_net_label, s_pair, s_ready, s_drive};
    for (int i = 0; i < 4; i++) {
        if (all[i] == o) lv_obj_remove_flag(all[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(all[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(o == s_drive ? YK_NIGHT : YK_PAPER), 0);
}

static void set_qr(const char *text) {
    if (!strcmp(text, s_qr_text)) return;
    snprintf(s_qr_text, sizeof(s_qr_text), "%s", text);
    lv_qrcode_update(s_qr, s_qr_text, strlen(s_qr_text));
}

/** The pairing page as it should be now: the hotspot's QR until the iPad joined it, then the page's. */
static void draw_pair(void) {
    char text[96], line[48];
    if (s_hotspot && !s_ap_client) {
        racer_wifi_qr(text, sizeof(text), s_ssid, s_key);
        set_qr(text);
        lv_label_set_text(s_pair_title, T("第 1 步 · 连上圆屏的热点", "Step 1 · Join the hotspot"));
        lv_label_set_text(s_pair_line, T("用 iPad 相机扫一扫", "Scan with the iPad camera"));
        snprintf(line, sizeof(line), "%s", s_ssid);
        lv_label_set_text(s_pair_d1, line);
        snprintf(line, sizeof(line), T("密码 %s", "Key %s"), s_key);
        lv_label_set_text(s_pair_d2, line);
        return;
    }
    racer_page_url(text, sizeof(text), s_ip, s_token);
    set_qr(text);
    if (s_lost) lv_label_set_text(s_pair_title, T("iPad 断开了，扫码再连", "iPad left · scan again"));
    else if (s_hotspot) lv_label_set_text(s_pair_title, T("第 2 步 · 打开赛道", "Step 2 · Open the track"));
    else lv_label_set_text(s_pair_title, T("连上 iPad", "Connect the iPad"));
    lv_label_set_text(s_pair_line, T("用 iPad 相机扫一扫", "Scan with the iPad camera"));
    if (s_hotspot) {
        lv_label_set_text(s_pair_d1, T("在 Safari 里打开", "Opens in Safari"));
        lv_label_set_text(s_pair_d2, s_ip);
    } else {
        lv_label_set_text(s_pair_d1, T("iPad 要连同一个 Wi-Fi", "iPad on the same Wi-Fi"));
        lv_label_set_text(s_pair_d2, s_hotspot_busy ? T("正在开热点…", "Opening hotspot…")
                                                     : T("点屏幕：改用圆屏热点", "Tap: use the hotspot"));
    }
}

static void build_screen(void) {
    s_screen = lv_obj_create(NULL);
    lv_obj_set_scrollable(s_screen, false);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(YK_PAPER), 0);
    s_net_label = yk_ui_label(s_screen, T("正在连网络…", "Connecting…"), 0, 214, 466, YK_SIZE_HEADING, YK_INK2);

    s_pair = layer(s_screen);
    s_pair_title = yk_ui_label(s_pair, "", 0, 62, 466, YK_SIZE_STATUS, YK_SAGE_TEXT);
    s_qr = lv_qrcode_create(s_pair);
    lv_qrcode_set_size(s_qr, 190);
    lv_qrcode_set_dark_color(s_qr, lv_color_hex(YK_NIGHT));
    lv_qrcode_set_light_color(s_qr, lv_color_hex(YK_INK));
    lv_obj_set_pos(s_qr, 138, 108);
    lv_obj_set_style_border_color(s_qr, lv_color_hex(YK_INK), 0);
    lv_obj_set_style_border_width(s_qr, 8, 0);
    s_pair_line = yk_ui_label(s_pair, "", 0, 318, 466, 22, YK_INK);
    s_pair_d1 = yk_ui_label(s_pair, "", 0, 352, 466, YK_SIZE_MIN, YK_INK2);
    s_pair_d2 = yk_ui_label(s_pair, "", 0, 376, 466, YK_SIZE_MIN, YK_INK2);

    s_ready = layer(s_screen);
    yk_ui_label(s_ready, T("iPad 连上了", "iPad connected"), 0, 110, 466, YK_SIZE_STATUS, YK_SAGE_TEXT);
    yk_ui_label(s_ready, T("像方向盘一样握住圆屏", "Hold it like a wheel"), 0, 170, 466, YK_SIZE_HEADING, YK_INK);
    yk_ui_label(s_ready, T("短按侧键开始", "Tap the side key to go"), 0, 214, 466, YK_SIZE_HEADING, YK_CORAL_TEXT);
    yk_ui_label(s_ready, T("左右转：转向", "Turn: steer"), 0, 286, 466, YK_SIZE_MIN, YK_INK2);
    yk_ui_label(s_ready, T("上边往前倾：加速 · 往后仰：刹车", "Lean away: gas · back: brake"), 0, 312, 466,
                YK_SIZE_MIN, YK_INK2);

    s_drive = layer(s_screen);
    s_speed = yk_ui_label(s_drive, "0", 0, 44, 466, 72, YK_INK);
    yk_ui_label(s_drive, "km/h", 0, 132, 466, YK_SIZE_STATUS, YK_INK2);
    s_pad_ring = dot(s_drive, PAD_X, PAD_Y, PAD_R, 0, 4, YK_PANEL);
    static lv_point_precise_t cross[2][2] = {{{PAD_X - PAD_R + 40, PAD_Y}, {PAD_X + PAD_R - 40, PAD_Y}},
                                             {{PAD_X, PAD_Y - PAD_R + 40}, {PAD_X, PAD_Y + PAD_R - 40}}};
    lv_line_set_points(stroke(s_drive, 3, YK_PANEL), cross[0], 2);
    lv_line_set_points(stroke(s_drive, 3, YK_PANEL), cross[1], 2);
    chevron(s_drive, 0, YK_SAGE);
    chevron(s_drive, 1, YK_INK3);
    chevron(s_drive, 2, YK_CORAL);
    chevron(s_drive, 3, YK_INK3);
    dot(s_drive, PAD_X, PAD_Y, HUB_R, YK_PAPER, 4, YK_INK2);
    s_arrow = stroke(s_drive, 8, YK_CREAM);
    s_arrow_pts[0].x = s_arrow_pts[1].x = PAD_X, s_arrow_pts[0].y = s_arrow_pts[1].y = PAD_Y;
    lv_line_set_points_mutable(s_arrow, s_arrow_pts, 2);
    s_ball = dot(s_drive, PAD_X, PAD_Y, BALL_R, YK_CORAL, 3, YK_CREAM);
    s_status = yk_ui_label(s_drive, "", 0, 386, 466, YK_SIZE_STATUS, YK_INK3);
}

static void enter(state_t st) {
    s_state = st;
    if (st == ST_PAIR) show_only(s_pair), draw_pair();
    else if (st == ST_READY) show_only(s_ready), yk_lan_send("{\"t\":\"state\",\"s\":\"ready\"}");
    else if (st == ST_DRIVE) {
        show_only(s_drive);
        s_want_neutral = true;
        s_drive_from = esp_timer_get_time() / 1000;
        yk_lan_send("{\"t\":\"state\",\"s\":\"drive\"}");
    }
    yk_app_keep_screen_on(st == ST_DRIVE || st == ST_READY);
    printf("DIAL_RACER_STATE %d\n", (int)st);
}

static void stop_driving(void) {
    if (s_state == ST_DRIVE) s_drive_ms += esp_timer_get_time() / 1000 - s_drive_from;
}

/** The ball goes where the screen leans (right = steer, up = gas), the arrow from the hub points at it. */
static void draw_pad(void) {
    float sx = s_tilt.steer, py = -s_tilt.push;
    const float m = sqrtf(sx * sx + py * py);
    if (m > 1) sx /= m, py /= m;
    const int bx = PAD_X + (int)(sx * PAD_REACH), by = PAD_Y + (int)(py * PAD_REACH);
    if (bx == s_shown_bx && by == s_shown_by) return;
    s_shown_bx = bx, s_shown_by = by;
    lv_obj_set_pos(s_ball, bx - BALL_R, by - BALL_R);
    const float d = sqrtf((float)((bx - PAD_X) * (bx - PAD_X) + (by - PAD_Y) * (by - PAD_Y)));
    if (d < HUB_R + 6) {
        lv_obj_add_flag(s_arrow, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const float ux = (bx - PAD_X) / d, uy = (by - PAD_Y) / d;
    s_arrow_pts[0].x = PAD_X + ux * HUB_R, s_arrow_pts[0].y = PAD_Y + uy * HUB_R;
    s_arrow_pts[1].x = bx - ux * BALL_R, s_arrow_pts[1].y = by - uy * BALL_R;
    lv_line_set_points_mutable(s_arrow, s_arrow_pts, 2);
    lv_obj_remove_flag(s_arrow, LV_OBJ_FLAG_HIDDEN);
}

/** A crash, a coin or a fork on the iPad: a line under the pad for a moment, and the ring flashes. */
static void note_events(const racer_tele_t *t, int64_t now) {
    const char *text = NULL;
    uint32_t color = YK_INK;
    if (t->hit > s_seen.hit) text = T("砰！撞到了", "Bang! A crash"), color = YK_CORAL;
    else if (t->fork > s_seen.fork) text = T("换赛道！", "New track!"), color = YK_SAGE;
    else if (t->coin > s_seen.coin) {
        snprintf(s_event, sizeof(s_event), T("金币 +1 · 共 %d", "Coin +1 · %d"), t->coin);
        color = YK_AMBER;
    }
    const bool reset = t->hit < s_seen.hit || t->coin < s_seen.coin || t->fork < s_seen.fork;
    s_seen = *t;
    if (reset || color == YK_INK) return;
    if (text) snprintf(s_event, sizeof(s_event), "%s", text);
    s_event_until = now + (color == YK_AMBER ? 700 : 1200);
    lv_obj_set_style_border_color(s_pad_ring, lv_color_hex(color), 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
    lv_label_set_text(s_status, s_event);
    s_shown_status = s_event;
}

static void draw_drive(void) {
    draw_pad();
    const racer_tele_t t = s_tele;
    const int64_t now = esp_timer_get_time() / 1000;
    note_events(&t, now);
    if (s_event_until && now >= s_event_until) {
        s_event_until = 0;
        lv_obj_set_style_border_color(s_pad_ring, lv_color_hex(YK_PANEL), 0);
        s_shown_status = NULL;
    }
    if (!s_event_until) {
        const char *status = !s_motion_ok ? T("没找到姿态传感器", "No motion sensor")
                             : t.off_road ? T("压到路边了", "Off the road")
                             : t.drift    ? T("漂移中", "Drifting")
                                          : T("侧键摆正 · 按住漂移", "Key: recenter · hold: drift");
        if (status != s_shown_status) {
            const uint32_t color = t.off_road || !s_motion_ok ? YK_CORAL_TEXT : t.drift ? YK_CREAM : YK_INK3;
            lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
            lv_label_set_text(s_status, s_shown_status = status);
        }
    }
    if (t.kmh != s_shown_kmh) {
        char v[8];
        snprintf(v, sizeof(v), "%d", s_shown_kmh = t.kmh);
        lv_label_set_text(s_speed, v);
        if (t.kmh > s_top_kmh) s_top_kmh = t.kmh;
    }
}

/** LVGL timer, 30 times a second: the state, the screen, the input to the iPad. */
static void tick(lv_timer_t *timer) {
    (void)timer;
    if (s_state == ST_NET) {
        if (s_net_up) enter(ST_PAIR);
        return;
    }
    if (s_closed) {
        s_closed = s_hello = false;
        stop_driving();
        s_lost = true;
        enter(ST_PAIR);
    }
    if (s_hello) {
        s_hello = false;
        s_lost = false;
        enter(ST_READY);
    }
    if (s_want_hotspot) {
        s_want_hotspot = false;
        if (!s_hotspot && !s_hotspot_busy) {
            s_hotspot_busy = true;
            xTaskCreate(hotspot_task, "racer_ap", 4096, NULL, 5, NULL);
        }
    }
    if (s_want_start) {
        s_want_start = false;
        if (s_state == ST_READY) enter(ST_DRIVE);
        else if (s_state == ST_DRIVE) s_want_neutral = true;
    }
    if (s_state == ST_PAIR) {
        draw_pair();
    } else if (s_state == ST_DRIVE) {
        draw_drive();
        char msg[64];
        if (racer_input_json(msg, sizeof(msg), s_tilt.steer, s_tilt.push, s_key_hold || s_touch_hold))
            yk_lan_send(msg);
    }
}

static void on_touch(lv_event_t *e) {
    const lv_event_code_t code = lv_event_get_code(e);
    yk_app_input();
    if (code == LV_EVENT_PRESSED) s_touch_hold = s_state == ST_DRIVE;
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) s_touch_hold = false;
    else if (code == LV_EVENT_CLICKED) {
        if (s_state == ST_PAIR && !s_hotspot) s_want_hotspot = true;
        else if (s_state == ST_READY) s_want_start = true;
    }
}

/** The side key (yaykit_key's task: flags only). A double click is the system's: the app ring. */
static bool on_key(const yk_key_event_t *ev, void *ctx) {
    (void)ctx;
    switch (ev->type) {
    case YK_KEY_TAP:
        s_want_start = true;
        return true;
    case YK_KEY_HOLD:
        s_key_hold = true;
        return true;
    case YK_KEY_UP:
        s_key_hold = false;
        return false;
    default:
        return false;
    }
}

/** Leaving for the main firmware: what the buddy says when it is back. Nothing is kept or recorded. */
static void on_leave(void *ctx) {
    (void)ctx;
    stop_driving();
    const int minutes = (int)((s_drive_ms + 30000) / 60000);
    if (!s_drive_ms || !s_top_kmh) return;
    yk_summary_t summary;
    yk_summary_init(&summary);
    char value[12], sentence[64];
    snprintf(value, sizeof(value), "%d", s_top_kmh);
    yk_summary_stat(&summary, value, "km/h");
    snprintf(value, sizeof(value), "%d", minutes < 1 ? 1 : minutes);
    yk_summary_stat(&summary, value, T("分钟", "min"));
    snprintf(sentence, sizeof(sentence), T("开了 %d 分钟赛车，最快 %d km/h", "Raced %d min, top speed %d km/h"),
             minutes < 1 ? 1 : minutes, s_top_kmh);
    yk_app_set_result_summary(sentence, &summary, NULL);
}

static esp_err_t on_command(const char *name, const cJSON *args, cJSON *reply, const char **code, void *ctx) {
    (void)args, (void)ctx;
    yk_app_input();
    esp_err_t err = yk_app_command(name, args, reply, code);
    if (err != ESP_ERR_NOT_SUPPORTED) return err;
    if (!strcmp(name, "dialracer.status")) {
        static const char *names[] = {"net", "pair", "ready", "drive"};
        float sd = 0, pd = 0;
        yk_motion_state_t ms;
        yk_motion_state(&ms);
        racer_tilt_angles(&s_tilt, ms.now.gravity, &sd, &pd);
        cJSON_AddStringToObject(reply, "state", names[s_state]);
        cJSON_AddStringToObject(reply, "net", s_hotspot ? "hotspot" : "wifi");
        cJSON_AddBoolToObject(reply, "motion", s_motion_ok);
        cJSON_AddBoolToObject(reply, "ipad", yk_lan_client());
        cJSON_AddNumberToObject(reply, "steer", s_tilt.steer);
        cJSON_AddNumberToObject(reply, "push", s_tilt.push);
        cJSON_AddNumberToObject(reply, "steerDeg", sd);
        cJSON_AddNumberToObject(reply, "pushDeg", pd);
        cJSON_AddNumberToObject(reply, "kmh", s_tele.kmh);
        return ESP_OK;
    }
    if (!strcmp(name, "dialracer.start")) {
        s_want_start = true;
        return ESP_OK;
    }
    *code = "UNKNOWN_COMMAND";
    return ESP_ERR_NOT_SUPPORTED;
}

static int heap_cmd(int argc, char **argv) {
    (void)argc, (void)argv;
    printf("HEAP internal=%u internal_min=%u psram=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return 0;
}

static void start_console(void) {
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    cfg.prompt = APP_SLUG ">";
    cfg.task_stack_size = 8192;
    esp_console_dev_usb_serial_jtag_config_t usb = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usb, &cfg, &repl));
    esp_console_register_help_command();
    ESP_ERROR_CHECK(yk_link_register_console());
    ESP_ERROR_CHECK(yk_app_register_console());
    yk_ui_register_snapshot();
    const esp_console_cmd_t heap = {.command = "heap", .help = "available memory", .func = heap_cmd};
    ESP_ERROR_CHECK(esp_console_cmd_register(&heap));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) ESP_LOGE(TAG, "NVS unavailable (%s); running without saved settings", esp_err_to_name(err));
    ESP_ERROR_CHECK(yk_app_start(&(yk_app_config_t){.slug = APP_SLUG, .version = APP_VERSION}));
    yk_app_on_leave(on_leave, NULL);
    if (!board_display_start_opts(&(board_display_opts_t){.start_dark = true})) {
        ESP_LOGE(TAG, "display failed");
        return;
    }
    ESP_ERROR_CHECK(yk_key_init(&(yk_key_config_t){.delay_tap = true}));
    ESP_ERROR_CHECK(yk_app_on_key(on_key, NULL));
    const yk_link_config_t link = {.board = "tikpal-r1", .app_slug = APP_SLUG, .app_version = APP_VERSION,
                                   .layout = "yaykit-v1", .idle_seconds = 90, .on_command = on_command};
    ESP_ERROR_CHECK(yk_link_init(&link));
    yk_launch_t launch;
    yk_app_launch(&launch);  // no launch parameters: read only so it does not linger
    s_en = yk_app_english();
    racer_tilt_init(&s_tilt);

    board_lock(-1);
    ESP_ERROR_CHECK(cjk_font_init());
    yk_ui_set_english(s_en);
    build_screen();
    const lv_event_code_t touch[] = {LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST, LV_EVENT_CLICKED};
    for (int i = 0; i < 4; i++) lv_obj_add_event_cb(s_screen, on_touch, touch[i], NULL);
    show_only(s_net_label);
    lv_screen_load(s_screen);
    lv_refr_now(NULL);
    board_brightness_set(yk_app_brightness());
    if (yk_hint_should(YK_HINT_APPS)) yk_hint_show(s_screen, YK_HINT_APPS, NULL);
    lv_timer_create(tick, 33, NULL);
    board_unlock();

    start_console();
    yk_app_ready();

    s_motion_ok = yk_motion_start(NULL) == ESP_OK;
    if (s_motion_ok) yk_motion_stream(on_samples, NULL);
    else ESP_LOGE(TAG, "no motion sensor");
    xTaskCreate(net_task, "racer_net", 4096, NULL, 5, NULL);
}
