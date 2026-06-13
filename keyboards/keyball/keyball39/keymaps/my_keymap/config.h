/*
This is the c configuration file for the keymap

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

#pragma once

#ifdef RGBLIGHT_ENABLE
#    define RGBLIGHT_EFFECT_BREATHING
#    define RGBLIGHT_EFFECT_RAINBOW_MOOD
#    define RGBLIGHT_EFFECT_RAINBOW_SWIRL
#    define RGBLIGHT_EFFECT_SNAKE
#    define RGBLIGHT_EFFECT_KNIGHT
#    define RGBLIGHT_EFFECT_CHRISTMAS
#    define RGBLIGHT_EFFECT_STATIC_GRADIENT
#    define RGBLIGHT_EFFECT_RGB_TEST
#    define RGBLIGHT_EFFECT_ALTERNATING
#    define RGBLIGHT_EFFECT_TWINKLE
#endif

#define TAP_CODE_DELAY 5

#define POINTING_DEVICE_AUTO_MOUSE_ENABLE
#define AUTO_MOUSE_DEFAULT_LAYER 1
#define AUTO_MOUSE_TIME 650
#define DYNAMIC_KEYMAP_LAYER_COUNT 7
#define AUTO_MOUSE_LAYER_KEEP_TIME 30000
#define TAPPING_TERM 200
#undef MOUSE_EXTENDED_REPORT
#undef WHEEL_EXTENDED_REPORT
// Add our own layer-sync RPC transaction alongside holykeebs' HK_SYNC_STATE
#undef SPLIT_TRANSACTION_IDS_USER
#define SPLIT_TRANSACTION_IDS_USER HK_SYNC_STATE, MY_LAYER_SYNC
#define KEYBALL_SCROLL_DIV_DEFAULT 4
#define KEYBALL_CPI_DEFAULT 1200

// Tapping term is stored at bytes 1020-1023 (value + 0xBEEF magic).
// Cap VIA macro storage at 1019 so dynamic_keymap_macro_reset() never zeroes those bytes.
#define DYNAMIC_KEYMAP_EEPROM_MAX_ADDR 1019

