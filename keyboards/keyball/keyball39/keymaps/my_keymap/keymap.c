/*
Copyright 2022 @Yowkees
Copyright 2022 MURAOKA Taro (aka KoRoN, @kaoriya)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include QMK_KEYBOARD_H
#include "quantum.h"
#include "eeprom.h"
#include "transactions.h"
#include <lib/lib8tion/lib8tion.h>

// Layer + mods synced to slave via MY_LAYER_SYNC RPC (2 bytes: [layer, mods]).
static uint8_t slave_layer = 0;
static uint8_t slave_mods  = 0;

static void layer_sync_handler(uint8_t len, const void *data, uint8_t rlen, void *rbuf) {
    if (len >= 2) {
        const uint8_t *d = (const uint8_t *)data;
        slave_layer = d[0];
        slave_mods  = d[1];
    }
}

// ---------------------------------------------------------------------------
// Custom Auto Mouse Layer (AML) behavior — ported from lib/keyball customization
//   - Trackball movement crossing threshold → activate AML, stay indefinitely.
//   - Mouse button clicks do NOT exit AML.
//   - Only ESC_ML (keyball_escape_mouse_layer) explicitly exits AML.
// All logic lives here in the keymap so the holykeebs keyball lib stays untouched.
// ---------------------------------------------------------------------------
#ifdef POINTING_DEVICE_AUTO_MOUSE_ENABLE
#include "pointing_device_auto_mouse.h"

static const uint16_t AML_ACTIVATE_THRESHOLD_C = 50;
static const uint16_t AML_NO_MOVE_RESET_MS     = 500;
#define AML_FORCED_EXIT_COOLDOWN 600

static uint16_t aml_no_move_timer     = 0;
static bool     aml_forced_exit       = false;
static uint16_t aml_forced_exit_timer = 0;

// Magnitude of pointer movement in this report.
static uint16_t aml_movement_size(report_mouse_t *r) {
    int16_t x = r->x < 0 ? -r->x : r->x;
    int16_t y = r->y < 0 ? -r->y : r->y;
    return (uint16_t)(x + y);
}

// Override QMK weak function. Returning true keeps the AML timer reset so the
// timeout never fires; AML stays active until keyball_escape_mouse_layer().
bool auto_mouse_activation(report_mouse_t mouse_report) {
    if (aml_forced_exit) {
        if (timer_elapsed(aml_forced_exit_timer) > AML_FORCED_EXIT_COOLDOWN) {
            aml_forced_exit = false;
        } else {
            return false;
        }
    }

    uint16_t movement = aml_movement_size(&mouse_report);
    static uint16_t total = 0;
    if (movement > 0) {
        aml_no_move_timer = 0;
        total += movement;
    } else if (!layer_state_is(AUTO_MOUSE_DEFAULT_LAYER)) {
        if (aml_no_move_timer == 0) {
            aml_no_move_timer = timer_read();
        } else if (timer_elapsed(aml_no_move_timer) > AML_NO_MOVE_RESET_MS) {
            total = 0;
            aml_no_move_timer = 0;
        }
    }

    if (AML_ACTIVATE_THRESHOLD_C < total) {
        total = 0;
        return true;
    }
    if (layer_state_is(AUTO_MOUSE_DEFAULT_LAYER)) {
        return true;  // keep AML alive regardless of clicks
    }
    return mouse_report.buttons;
}

// Explicitly exit AML and suppress re-activation for a short cooldown.
void keyball_escape_mouse_layer(void) {
    layer_off(get_auto_mouse_layer());
    aml_forced_exit       = true;
    aml_forced_exit_timer = timer_read();
    aml_no_move_timer     = 0;
}

// Called from layer_state_set_user on every layer change. Previously lived in
// the keyball lib; reduced here to a no-op-safe stub (movement counter is local).
void keyball_handle_auto_mouse_layer_change(layer_state_t state) {
    (void)state;
}
#endif

// Custom EEPROM layout for tapping term (outside eeconfig/VIA/keymap areas).
// Bytes 1020-1021: saved tapping term value (uint16_t)
// Bytes 1022-1023: magic sentinel — 0xBEEF confirms we wrote these bytes
// config.h sets DYNAMIC_KEYMAP_EEPROM_MAX_ADDR 1019 so VIA macro reset never touches them.
#define TT_EEPROM_VAL   ((uint16_t*)1020)
#define TT_EEPROM_MAGIC ((uint16_t*)1022)
#define TT_MAGIC_VALUE  0xBEEF

static void tt_save(uint16_t val) {
    eeprom_update_word(TT_EEPROM_VAL, val);
    eeprom_update_word(TT_EEPROM_MAGIC, TT_MAGIC_VALUE);
}

static uint16_t tt_load(void) {
    if (eeprom_read_word(TT_EEPROM_MAGIC) == TT_MAGIC_VALUE) {
        uint16_t v = eeprom_read_word(TT_EEPROM_VAL);
        if (v >= 50 && v <= 2000) return v;
    }
    return TAPPING_TERM;
}

// Custom keycodes for escaping the auto mouse layer.
//   ESC_ML      - exit AML only
//   ESC_ML_LNG2 - exit AML then tap LNG2 (英数, switch to English input)
//   ESC_ML_LNG1 - exit AML then tap LNG1 (かな, switch to Japanese input)
//   MY_ANI      - cycle custom LED animation (OFF → PLASMA → AURORA → METEOR → OFF)
enum my_keycodes {
    ESC_ML = KEYBALL_SAFE_RANGE,
    ESC_ML_LNG2,
    ESC_ML_LNG1,
    MY_ANI,
};

// Deferred LNG key: sent on the next matrix scan after AML exit so that
// the layer change is fully committed before the HID report is generated.
static uint8_t pending_lang_key = 0;

void matrix_scan_user(void) {
    if (pending_lang_key) {
        tap_code(pending_lang_key);
        pending_lang_key = 0;
    }
}

// ── Custom per-LED animations ──────────────────────────────────────────────────
// MY_ANI cycles: OFF → PLASMA → AURORA → METEOR → OFF.
// Uses rgblight_driver.set_color() × 24 (buffer only, no flush) + rgblight_set()
// × 1 per frame → ~1.5ms DMA busy time, safe for keyboard scanning.
// Both sides render independently using their local 24-LED indices.

typedef enum {
    MY_ANIM_OFF = 0,
    MY_ANIM_PLASMA,   // multi-wave HSV interference: fast, vibrant, full-spectrum
    MY_ANIM_AURORA,   // slow cool-color bands: blue/teal/green drifting gently
    MY_ANIM_METEOR,   // three coloured shooting stars circling the strip
    MY_ANIM_COUNT,
} my_anim_mode_t;

static my_anim_mode_t my_anim       = MY_ANIM_OFF;
static uint16_t       my_anim_timer = 0;
static uint16_t       my_tick       = 0;

#define MY_ANIM_LEDS 24
#define MY_ANIM_MS   20

static void my_draw_anim(void) {
    uint8_t t = (uint8_t)my_tick;

    // Meteor comet parameters (PROGMEM to avoid stack)
    static const uint8_t PROGMEM met_spd[3] = {5, 7, 11};
    static const uint8_t PROGMEM met_hue[3] = {0, 85, 170};

    for (uint8_t i = 0; i < MY_ANIM_LEDS; i++) {
        uint8_t h = 0, s = 255, v = 0;

        switch (my_anim) {
            case MY_ANIM_PLASMA: {
                // Three overlapping sine waves → complex colour interference
                uint8_t w1 = sin8((uint8_t)(i * 8  + t));
                uint8_t w2 = sin8((uint8_t)(i * 13 + t + (t >> 1)));
                uint8_t w3 = sin8((uint8_t)(i * 3  - (t >> 1)));
                h = w1 / 3 + w2 / 3 + w3 / 3;
                v = 130 + sin8((uint8_t)((h >> 1) + t)) / 5;  // 130-181
                break;
            }
            case MY_ANIM_AURORA: {
                // Hue drifts in the blue-teal-green-purple band (130-193)
                h = 130 + sin8((uint8_t)(i * 6 + t / 3)) / 4;
                s = 220;
                v = 80 + sin8((uint8_t)(i * 9 + (t >> 1))) / 3;  // 80-165
                break;
            }
            case MY_ANIM_METEOR: {
                // Three coloured comets (R/G/B) with exponential tails
                for (uint8_t c = 0; c < 3; c++) {
                    uint8_t spd  = pgm_read_byte(&met_spd[c]);
                    uint8_t head = (uint8_t)(((uint16_t)t * spd >> 2) % MY_ANIM_LEDS);
                    uint8_t dist = (uint8_t)((head - i + MY_ANIM_LEDS) % MY_ANIM_LEDS);
                    if (dist < 6) {
                        uint8_t br = (uint8_t)(200 >> dist);  // 200→100→50→25→12→6
                        if (br > v) { h = pgm_read_byte(&met_hue[c]); v = br; }
                    }
                }
                break;
            }
            default: break;
        }

        hsv_t hsv = {h, s, v};
        rgb_t rgb = hsv_to_rgb(hsv);
        // set_color() only writes to buffer; rgblight_set() below does one DMA flush.
        rgblight_driver.set_color(i, rgb.r, rgb.g, rgb.b);
    }
    rgblight_set();
}

void housekeeping_task_user(void) {
    // Animation runs on both sides (each drives its own 24 LEDs)
    if (my_anim != MY_ANIM_OFF && timer_elapsed(my_anim_timer) >= MY_ANIM_MS) {
        my_anim_timer = timer_read();
        my_tick++;
        my_draw_anim();
    }

#if defined(SPLIT_KEYBOARD)
    if (is_keyboard_master()) {
        static uint16_t last_saved = TAPPING_TERM;
        if (g_tapping_term != last_saved) {
            tt_save(g_tapping_term);
            last_saved = g_tapping_term;
        }
        // Send layer + mods to slave OLED (2 bytes, only on change).
        static uint8_t last_layer = 255;
        static uint8_t last_mods  = 255;
        uint8_t l = get_highest_layer(layer_state);
        uint8_t m = get_mods();
        if (l != last_layer || m != last_mods) {
            uint8_t buf[2] = {l, m};
            if (transaction_rpc_send(MY_LAYER_SYNC, sizeof(buf), buf)) {
                last_layer = l;
                last_mods  = m;
            }
        }
    }
#endif
}


bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    switch (keycode) {
        case ESC_ML:
            if (record->event.pressed) {
#ifdef POINTING_DEVICE_AUTO_MOUSE_ENABLE
                keyball_escape_mouse_layer();
#endif
            }
            return false;
        case ESC_ML_LNG2:
            if (record->event.pressed) {
#ifdef POINTING_DEVICE_AUTO_MOUSE_ENABLE
                keyball_escape_mouse_layer();
#endif
                pending_lang_key = KC_LNG2;
            }
            return false;
        case ESC_ML_LNG1:
            if (record->event.pressed) {
#ifdef POINTING_DEVICE_AUTO_MOUSE_ENABLE
                keyball_escape_mouse_layer();
#endif
                pending_lang_key = KC_LNG1;
            }
            return false;
        case MY_ANI:
            if (record->event.pressed) {
                my_anim = (my_anim_mode_t)((my_anim + 1) % MY_ANIM_COUNT);
                if (my_anim == MY_ANIM_OFF) {
                    // Re-apply the current layer's colour
                    layer_state_set_user(layer_state);
                } else {
                    // Freeze RGBLIGHT so its timer doesn't overwrite our per-LED work
                    rgblight_mode_noeeprom(RGBLIGHT_MODE_STATIC_LIGHT);
                }
            }
            return false;
    }
    return true;
}

// clang-format off
const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {

  [0] = LAYOUT_universal(
    KC_Q     , KC_W     , KC_E     , KC_R     , KC_T     ,                            KC_Y     , KC_U     , KC_I     , KC_O     , KC_P     ,
    KC_A     , KC_S     , KC_D     , KC_F     , KC_G     ,                            KC_H     , KC_J     , KC_K     , KC_L     , KC_SCLN  ,
    KC_Z     , KC_X     , KC_C     , KC_V     , KC_B     ,                            KC_N     , KC_M     , KC_COMM  , KC_DOT   , KC_SLSH  ,
    KC_LCTL  , KC_LGUI  , KC_LALT  , LSFT_T(KC_LNG2), LT(5,KC_SPC), LT(3,KC_LNG1),  KC_BSPC  , LT(2,KC_ENT), LSFT_T(KC_LNG2), KC_RALT, KC_RGUI, KC_RCTL
  ),

  [1] = LAYOUT_universal(
    LGUI(KC_C), LGUI(KC_V), LGUI(KC_X), KC_DEL  , LT(3,KC_NO),                       KC_NO    , KC_BTN1  , LALT(KC_W), KC_ESC   , TG(0)    ,
    KC_NO     , LCTL(KC_C), LCTL(KC_UP), LCTL(KC_V), LCTL(KC_LEFT),                  LCTL(KC_RGHT), KC_BTN1, KC_UP   , KC_BTN2  , TG(1)    ,
    KC_NO     , LCTL(KC_X), KC_NO     , KC_NO    , LCTL(KC_DOWN),                     KC_NO    , KC_LEFT  , KC_DOWN  , KC_RGHT  , TG(1)    ,
    KC_LCTL   , KC_NO     , KC_NO     , ESC_ML_LNG2, LT(3,KC_NO), ESC_ML_LNG1,        KC_NO    , LT(1,KC_NO), KC_NO  , KC_RALT  , KC_RGUI  , KC_LCTL
  ),

  [2] = LAYOUT_universal(
    LGUI(KC_C), KC_UP     , LGUI(KC_V), LCTL(KC_PGUP), LGUI(KC_X),                   LALT(KC_GRV), LGUI(KC_W), LCTL(KC_PGDN), KC_TAB  , LGUI(KC_T),
    LGUI(KC_F), LCTL(KC_LEFT), LGUI(KC_DOWN), LCTL(KC_RGHT), LSFT(KC_W),             LCTL(KC_LNG1), KC_PGUP  , KC_LNG1  , KC_4    , LSFT_T(KC_NO),
    LCTL(KC_C), LCTL(KC_UP), LCTL(KC_V), LCTL(KC_LEFT), LGUI(KC_LBRC),               KC_PGDN  , LCTL(KC_X), KC_NO    , LGUI(LSFT(KC_5)), LGUI(KC_MINS),
    KC_LCTL   , KC_LGUI   , KC_NO     , KC_DEL   , LT(2,KC_NO), KC_BSPC ,             KC_NO    , KC_NO     , KC_NO   , KC_NO    , LGUI(KC_0), KC_NO
  ),

  [3] = LAYOUT_universal(
    RGB_TOG  , AML_TO   , AML_I50  , AML_D50  , MY_ANI   ,                            DT_DOWN  , DT_UP    , SSNP_HOR , SSNP_VRT , SSNP_FRE ,
    RGB_MOD  , RGB_HUI  , RGB_SAI  , RGB_VAI  , KBC_SAVE ,                            KC_NO    , KC_NO    , KC_NO    , KC_NO    , KC_NO    ,
    RGB_RMOD , RGB_HUD  , RGB_SAD  , RGB_VAD  , KC_NO    ,                            CPI_D1K  , CPI_D100 , CPI_I100 , CPI_I1K  , KBC_SAVE ,
    QK_BOOT  , KBC_RST  , KC_NO    , KC_NO    , KC_NO    , KC_NO    ,      KC_LNG2  , KC_LNG1  , KC_NO    , KC_NO    , KBC_RST  , QK_BOOT
  ),

  [4] = LAYOUT_universal(
    KC_NO    , KC_F3    , KC_NO    , KC_F4    , KC_NO    ,                            KC_NO    , KC_F7    , KC_NO    , KC_NO    , KC_NO    ,
    KC_F1    , KC_NO    , KC_NO    , KC_NO    , KC_NO    ,                            KC_F8    , KC_NO    , KC_F9    , KC_NO    , KC_NO    ,
    KC_NO    , KC_NO    , KC_NO    , KC_NO    , KC_NO    ,                            KC_NO    , KC_NO    , KC_NO    , KC_NO    , KC_F11   ,
    KC_NO    , KC_NO    , KC_NO    , KC_NO    , KC_NO    , KC_NO    ,      KC_NO    , KC_NO    , KC_NO    , KC_NO    , KC_NO    , KC_NO
  ),

  [5] = LAYOUT_universal(
    KC_EXLM  , KC_AT    , KC_HASH  , KC_DLR   , KC_PERC  ,                            KC_CIRC  , KC_AMPR  , KC_ASTR  , KC_LPRN  , KC_RPRN  ,
    KC_1     , KC_2     , KC_3     , KC_4     , KC_5     ,                            KC_6     , KC_7     , KC_8     , KC_9     , KC_0     ,
    KC_BSLS  , KC_LBRC  , KC_RBRC  , KC_MINS  , KC_EQL   ,                            KC_GRV   , KC_QUOT  , KC_DQUO  , KC_TILD  , KC_NO    ,
    KC_NO    , KC_NO    , KC_NO    , KC_NO    , KC_NO    , KC_NO    ,      KC_NO    , KC_NO    , KC_NO    , KC_NO    , KC_NO    , KC_NO
  ),

  [6] = LAYOUT_universal(
    KC_TRNS  , KC_TRNS  , KC_TRNS  , KC_TRNS  , KC_TRNS  ,                            KC_TRNS  , KC_TRNS  , KC_TRNS  , KC_TRNS  , KC_TRNS  ,
    KC_TRNS  , KC_TRNS  , KC_TRNS  , KC_TRNS  , KC_TRNS  ,                            KC_TRNS  , KC_TRNS  , KC_TRNS  , KC_TRNS  , KC_TRNS  ,
    KC_TRNS  , KC_TRNS  , KC_TRNS  , KC_TRNS  , KC_TRNS  ,                            KC_TRNS  , KC_TRNS  , KC_TRNS  , KC_TRNS  , KC_TRNS  ,
    KC_TRNS  , KC_TRNS  , KC_TRNS  , KC_TRNS  , KC_TRNS  , KC_TRNS  ,      KC_TRNS  , KC_TRNS  , KC_TRNS  , KC_TRNS  , KC_TRNS  , KC_TRNS
  ),
};
// clang-format on

// Override weak function from keyball.c to preserve division remainder across
// report cycles. The original discards the remainder each cycle, causing a
// dead zone at the start of every scroll gesture.
void keyball_on_apply_motion_to_mouse_scroll(report_mouse_t *report, report_mouse_t *output, bool is_left) {
    static int16_t rem_h = 0;
    static int16_t rem_v = 0;

    int16_t div = 1 << (keyball_get_scroll_div() - 1);
    int16_t x   = report->x + rem_h;
    int16_t y   = report->y + rem_v;
    int16_t qx  = x / div;
    int16_t qy  = y / div;
    rem_h = x - qx * div;
    rem_v = y - qy * div;

    int8_t cx = qx > 127 ? 127 : qx < -127 ? -127 : (int8_t)qx;
    int8_t cy = qy > 127 ? 127 : qy < -127 ? -127 : (int8_t)qy;

    output->h = -cx;
    output->v = cy;
    if (is_left) {
        output->h = -output->h;
        output->v = -output->v;
    }
}

void keyboard_post_init_user(void) {
    dprintf("is_master: %d\n", is_keyboard_master());
    if (!is_keyboard_master()) {
        transaction_register_rpc(MY_LAYER_SYNC, layer_sync_handler);
        return;
    }
    set_auto_mouse_layer(1);
    set_auto_mouse_enable(true);
#ifdef DYNAMIC_TAPPING_TERM_ENABLE
    g_tapping_term = tt_load();
#endif
}

layer_state_t layer_state_set_user(layer_state_t state) {
  #ifdef POINTING_DEVICE_AUTO_MOUSE_ENABLE
    keyball_handle_auto_mouse_layer_change(state); // ← これを追加
  #endif
    // Layer 3 のときスクロールモード ON
    keyball_set_scroll_mode(get_highest_layer(state) == 3);

    // 現在の明るさを維持しつつレイヤーで色を変える
    uint8_t current_val = rgblight_get_val();
    uint8_t hue = 0;

    switch (get_highest_layer(state)) {
        case 0: hue =  85; break; // 蛍光グリーン（デフォルト）
        case 1: hue = 128; break; // 蛍光シアン（マウス・AML）
        case 2: hue =  43; break; // 蛍光イエロー
        case 3: hue =  21; break; // 蛍光オレンジ（設定・スクロール）
        case 4: hue = 213; break; // 蛍光ピンク（ファンクション）
        case 5: hue = 192; break; // 蛍光パープル（記号・数字）
        case 6: hue =   0; break; // 蛍光レッド（未使用）
        default: hue =  85; break;
    }
    if (my_anim == MY_ANIM_OFF) {
        rgblight_sethsv_noeeprom(hue, 255, current_val);
    }

    return state;
}

#ifdef OLED_ENABLE
#    include "lib/oledkit/oledkit.h"

// Right-justify unsigned 16-bit value in a 4-char space-padded field.
static void oled_write_num4(uint16_t n) {
    char    buf[5] = "    ";
    uint8_t i      = 4;
    buf[i]         = '\0';
    if (n == 0) {
        buf[--i] = '0';
    } else {
        while (n > 0 && i > 0) {
            buf[--i] = '0' + (n % 10);
            n /= 10;
        }
    }
    oled_write(buf, false);
}

// Lower nibble of x as a hex digit.
static char nibble_hex(uint8_t x) {
    x &= 0x0f;
    return x < 10 ? x + '0' : x + 'a' - 10;
}

// Both OLEDs mounted vertically, connector pins at the bottom.
// OLED_ROTATION_270 renders text top-to-bottom in portrait orientation.
oled_rotation_t oled_init_user(oled_rotation_t rotation) {
    return OLED_ROTATION_270;
}

// Bitmap tables – each byte: bit4=col0(left) … bit0=col4(right).
// Layer digits: 7 digits × 3 rows (compact, placed at OLED bottom).
// Bit layout per row: bit4=col0(left) … bit0=col4(right).
//   .###. = 14   .#.#. = 10   ..##. = 6   .##.. = 12   ...#. = 2   ..#.. = 4
static const uint8_t PROGMEM big_digit_data[7][3] = {
    {14, 10, 14},  // 0: .###. / .#.#. / .###.
    { 4,  4, 14},  // 1: ..#.. / ..#.. / .###.
    {14,  6, 12},  // 2: .###. / ..##. / .##..
    {14,  6, 14},  // 3: .###. / ..##. / .###.
    {10, 14,  2},  // 4: .#.#. / .###. / ...#.
    {12, 14,  6},  // 5: .##.. / .###. / ..##.
    {14, 12, 14},  // 6: .###. / .##.. / .###.
};

// Layer digits: 7 digits × 5 rows (large, for slave OLED).
//   #...# = 17   ####. = 30   ##### = 31   #.... = 16   ....# = 1
static const uint8_t PROGMEM big_digit_data5[7][5] = {
    {14, 17, 17, 17, 14},  // 0: .###. / #...# / #...# / #...# / .###.
    {12,  4,  4,  4, 14},  // 1: .##.. / ..#.. / ..#.. / ..#.. / .###.
    {14,  1,  6, 16, 31},  // 2: .###. / ....# / ..##. / #.... / #####
    {14,  1,  6,  1, 14},  // 3: .###. / ....# / ..##. / ....# / .###.
    {17, 17, 31,  1,  1},  // 4: #...# / #...# / ##### / ....# / ....#
    {31, 16, 30,  1, 14},  // 5: ##### / #.... / ####. / ....# / .###.
    {14, 16, 30, 17, 14},  // 6: .###. / #.... / ####. / #...# / .###.
};

// Renders a 5-wide bitmap from PROGMEM over `rows` display rows.
// Blank rows (data byte = 0) are never inverted, keeping them dark regardless of pressed.
static void oled_write_big_bitmap(const uint8_t *data, uint8_t rows, bool pressed) {
    for (uint8_t r = 0; r < rows; r++, data++) {
        uint8_t b = pgm_read_byte(data);
        bool inv = pressed && b;
        uint8_t m = 0x10;
        for (uint8_t c = 0; c < 5; c++) {
            oled_write_char(b & m ? '#' : ' ', inv);
            m >>= 1;
        }
    }
}


// 5-wide × 5-row mod letter bitmaps (used when 1-2 mods active, 10 rows available).
static const uint8_t PROGMEM big_mod_C5[5] = {14, 16, 16, 16, 14}; // .###./#..../#..../#..../.###.
static const uint8_t PROGMEM big_mod_S5[5] = {15, 16, 14,  1, 30}; // .####/#..../.###./....#/####.
static const uint8_t PROGMEM big_mod_A5[5] = { 4, 10, 31, 17, 17}; // ..#../.#.#./#####/#...#/#...#
static const uint8_t PROGMEM big_mod_G5[5] = {14, 16, 19, 17, 15}; // .###./#..../#..##/#...#/.####

// 5-wide × 3-row mod letter bitmaps (compact, used when 3-4 mods active).
static const uint8_t PROGMEM big_mod_C3[3] = {14, 16, 14}; // .###./#..../.###.
static const uint8_t PROGMEM big_mod_S3[3] = {30, 14, 15}; // ####./.###./.####
static const uint8_t PROGMEM big_mod_A3[3] = { 4, 31, 17}; // ..#../#####/#...#
static const uint8_t PROGMEM big_mod_G3[3] = {14, 19, 15}; // .###./#..##/.####

// ── Master OLED – key / ball / layer (portrait 5 chars/row) ──────────────────
// Note: writing exactly 5 chars auto-wraps to the next row via oled_advance_char().
// Do NOT call oled_advance_page() after a 5-char write — it would skip an extra row.

void oledkit_render_info_user(void) {
    // Row 0: key position  "R?C? "
    oled_write_char('R', false);
    oled_write_char(nibble_hex(keyball.last_pos.row), false);
    oled_write_char('C', false);
    oled_write_char(nibble_hex(keyball.last_pos.col), false);
    oled_write_char(' ', false);

    // Row 1: keycode  "K??  "
    oled_write_char('K', false);
    oled_write_char(nibble_hex(keyball.last_kc >> 4), false);
    oled_write_char(nibble_hex(keyball.last_kc), false);
    oled_write_P(PSTR("  "), false);

    // Row 2: pressing keys (always exactly 5 chars)
    for (uint8_t i = 0; i < 5; i++) oled_write_char(keyball.pressing_keys[i], false);

    // Rows 3-4: ball x, y  "x NNN" / "y NNN"
#define WB(label, val) do { \
    int8_t _v = (val); \
    oled_write_char((label), false); \
    oled_write_num4((uint16_t)(_v < 0 ? -(int)_v : (int)_v)); \
} while (0)
    WB('x', keyball.last_mouse.x);
    WB('y', keyball.last_mouse.y);
#undef WB

    // Row 5: CPI  "CNNNN"
    oled_write_char('C', false);
    oled_write_num4(keyball_get_cpi());

    // Row 6: AML status
#ifdef POINTING_DEVICE_AUTO_MOUSE_ENABLE
    if (!get_auto_mouse_enable()) {
        oled_write_P(PSTR("AmOff"), false);
    } else if (layer_state_is(AUTO_MOUSE_DEFAULT_LAYER)) {
        oled_write_P(PSTR("Am:AC"), false);
    } else {
        oled_write_P(PSTR("Am:On"), false);
    }
#endif

    // Row 7: tapping term  "T NNN"
    oled_write_char('T', false);
    oled_write_num4(g_tapping_term);

    // Rows 8-10: blank (former MOD rows removed)
    oled_write_P(PSTR("               "), false);

    // Rows 11-15: large 5-row layer digit (same style as slave OLED)
    {
        uint8_t l = get_highest_layer(layer_state);
        if (l < 7) {
            oled_write_big_bitmap(big_digit_data5[l], 5, false);
        } else {
            oled_write_P(PSTR("                         "), false);
        }
    }
}

// ── Slave OLED – active mod letters (top) + layer digit (bottom) ─────────────
// Rows 0-9:  active mod letters (10 rows); Div:N when no mods pressed.
//   1-2 mods → 5 rows each   3-4 mods → 10/n rows each (compact)
// Row 10:    blank separator (implicit via oled_clear + oled_set_cursor).
// Rows 11-15: large 5-row layer digit.
void oledkit_render_logo_user(void) {
    oled_clear();

    // Rows 0-9: active mod letters.
    // combined = L|R for each mod: bit0=Ctrl bit1=Shift bit2=Alt bit3=GUI
    uint8_t cm = (slave_mods | (slave_mods >> 4)) & 0x0F;
    if (cm == 0) {
        oled_write_P(PSTR("Div:"), false);
        oled_write_char('0' + keyball_get_scroll_div(), false);
    } else {
        uint8_t n    = (uint8_t)__builtin_popcount(cm);
        uint8_t rows = (n <= 2) ? 5 : (10 / n);
        static const uint8_t * const mod5[4] = {big_mod_C5, big_mod_S5, big_mod_A5, big_mod_G5};
        static const uint8_t * const mod3[4] = {big_mod_C3, big_mod_S3, big_mod_A3, big_mod_G3};
        static const uint8_t mod_bit[4]      = {0x01, 0x02, 0x04, 0x08};
        for (uint8_t i = 0; i < 4; i++) {
            if (cm & mod_bit[i]) {
                oled_write_big_bitmap((n <= 2) ? mod5[i] : mod3[i], rows, false);
            }
        }
    }

    // Rows 11-15: large 5-row layer digit, fixed at bottom via oled_set_cursor.
    oled_set_cursor(0, 11);
    uint8_t l = slave_layer;
    if (l < 7) {
        oled_write_big_bitmap(big_digit_data5[l], 5, false);
    } else {
        oled_write_P(PSTR("                         "), false);
    }
}
#endif

const uint16_t PROGMEM df_combo[] = {KC_D, KC_F, COMBO_END};
const uint16_t PROGMEM jk_combo[] = {KC_J, KC_K, COMBO_END};

combo_t key_combos[] = {
    COMBO(df_combo, KC_LNG2),
    COMBO(jk_combo, KC_LNG1),
};

void process_combo_event(uint16_t combo_index, bool pressed) {
    if (pressed) {
        dprintf("combo fired: %u\n", combo_index);
    }
}
