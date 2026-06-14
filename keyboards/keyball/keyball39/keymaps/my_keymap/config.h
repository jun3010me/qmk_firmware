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
#define KEYBALL_CPI_DEFAULT 1100

// Tapping term is stored at bytes 1020-1023 (value + 0xBEEF magic).
// Cap VIA macro storage at 1019 so dynamic_keymap_macro_reset() never zeroes those bytes.
#define DYNAMIC_KEYMAP_EEPROM_MAX_ADDR 1019

// Disable split watchdog to prevent the secondary MCU from resetting during USB suspend.
//
// Root cause of the "CPI feels low after idle" bug:
//   1. Host suspends USB (display sleep) → master stops sending split sync packets.
//   2. Holykeebs sets SPLIT_WATCHDOG_TIMEOUT to 3000ms, so secondary resets after 3s.
//   3. Secondary reboots and reads CPI from its own EEPROM (flash).
//      On RP2040, each MCU has independent flash; VIA only writes to master's flash,
//      so secondary always restores to KEYBALL_CPI_DEFAULT (1200) after reset.
//   4. Master's split CPI sync only sends on change (last_cpi != shared_cpi).
//      Since the master's CPI hasn't changed, it never resyncs to the secondary.
//   5. User sees abnormally low CPI until USB is reconnected (which resets last_cpi).
//
// Fix: prevent secondary reset by disabling the watchdog.
// RP2040 is stable enough that a secondary hang requiring watchdog recovery is
// effectively never seen in practice.
#undef SPLIT_WATCHDOG_ENABLE

