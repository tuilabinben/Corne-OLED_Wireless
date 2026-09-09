/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display/status_screen.h>
#include <zmk/display/widgets/output_status.h>
#include <zmk/display/widgets/peripheral_status.h>
#include <zmk/display/widgets/layer_status.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/battery.h>
#include <zmk/display.h>

#if IS_ENABLED(CONFIG_ZMK_WPM)
#include <zmk/events/wpm_state_changed.h>
#include <zmk/wpm.h>
#endif

/* =========================================================================
 * Option 1: Audio Pulse / Oscilloscope Boot & Wake Animation
 * ========================================================================= */
#define NUM_WAVE_BARS 16
static lv_obj_t *intro_overlay = NULL;
static lv_timer_t *intro_timer = NULL;
static lv_obj_t *wave_bars[NUM_WAVE_BARS];
static uint8_t intro_step = 0;

/* Soundwave amplitude lookup table: flat -> spike burst -> settle */
static const int8_t wave_frames[10][NUM_WAVE_BARS] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 1, -2, 3, -5, 7, -8, 7, -5, 3, -2, 1, 0, 0, 0},
    {0, 1, -3, 6, -10, 13, -14, 13, -10, 7, -4, 2, -1, 0, 0, 0},
    {0, 0, 1, -4, 8, -13, 14, -13, 9, -6, 3, -1, 0, 0, 0, 0},
    {0, 0, 0, 1, -3, 6, -9, 11, -8, 5, -3, 1, 0, 0, 0, 0},
    {0, 0, 0, 0, 1, -2, 4, -6, 6, -4, 2, -1, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 1, -2, 3, -3, 2, -1, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 1, -1, 1, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
};

static void intro_dismiss(void) {
    if (intro_overlay != NULL) {
        lv_obj_del(intro_overlay);
        intro_overlay = NULL;
    }
    if (intro_timer != NULL) {
        lv_timer_del(intro_timer);
        intro_timer = NULL;
    }
}

static void intro_timer_cb(lv_timer_t *timer) {
    intro_step++;
    if (intro_step >= 12) {
        intro_dismiss();
        return;
    }

    if (intro_step < 10 && intro_overlay != NULL) {
        for (int i = 0; i < NUM_WAVE_BARS; i++) {
            int8_t offset = wave_frames[intro_step][i];
            int8_t h = offset >= 0 ? offset + 2 : (-offset) + 2;
            int8_t y = 15 - (offset > 0 ? offset : 0);
            if (wave_bars[i] != NULL) {
                lv_obj_set_size(wave_bars[i], 4, h);
                lv_obj_set_pos(wave_bars[i], 16 + (i * 6), y);
            }
        }
    }
}

static void start_intro_animation(lv_obj_t *parent) {
    intro_overlay = lv_obj_create(parent);
    lv_obj_clear_flag(intro_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(intro_overlay, 128, 32);
    lv_obj_set_pos(intro_overlay, 0, 0);
    lv_obj_set_style_bg_color(intro_overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(intro_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(intro_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(intro_overlay, 0, LV_PART_MAIN);

    /* Flat baseline across the center at Y=15 */
    lv_obj_t *baseline = lv_obj_create(intro_overlay);
    lv_obj_clear_flag(baseline, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(baseline, 128, 2);
    lv_obj_set_pos(baseline, 0, 15);
    lv_obj_set_style_bg_color(baseline, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(baseline, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(baseline, 0, LV_PART_MAIN);

    /* Waveform pulse segments */
    for (int i = 0; i < NUM_WAVE_BARS; i++) {
        wave_bars[i] = lv_obj_create(intro_overlay);
        lv_obj_clear_flag(wave_bars[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(wave_bars[i], 4, 2);
        lv_obj_set_pos(wave_bars[i], 16 + (i * 6), 15);
        lv_obj_set_style_bg_color(wave_bars[i], lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(wave_bars[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(wave_bars[i], 0, LV_PART_MAIN);
        lv_obj_set_style_radius(wave_bars[i], 1, LV_PART_MAIN);
    }

    intro_step = 0;
    intro_timer = lv_timer_create(intro_timer_cb, 55, NULL);
}

/* =========================================================================
 * Custom Battery Widget (Symbol Only)
 * ========================================================================= */
static lv_obj_t *battery_label = NULL;

typedef struct {
    uint8_t level;
} custom_battery_state_t;

static custom_battery_state_t custom_battery_get_state(const zmk_event_t *eh) {
    return (custom_battery_state_t){.level = zmk_battery_state_of_charge()};
}

static void custom_battery_update_cb(custom_battery_state_t state) {
    if (battery_label == NULL) {
        return;
    }
    const char *icon = LV_SYMBOL_BATTERY_FULL;
    if (state.level < 20) {
        icon = LV_SYMBOL_BATTERY_EMPTY;
    } else if (state.level < 40) {
        icon = LV_SYMBOL_BATTERY_1;
    } else if (state.level < 60) {
        icon = LV_SYMBOL_BATTERY_2;
    } else if (state.level < 80) {
        icon = LV_SYMBOL_BATTERY_3;
    }
    lv_label_set_text(battery_label, icon);
}

ZMK_DISPLAY_WIDGET_LISTENER(custom_battery_widget, custom_battery_state_t,
                            custom_battery_update_cb, custom_battery_get_state)
ZMK_SUBSCRIPTION(custom_battery_widget, zmk_battery_state_changed);

/* =========================================================================
 * Central vs Peripheral Layouts
 * ========================================================================= */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)

#if IS_ENABLED(CONFIG_ZMK_WIDGET_OUTPUT_STATUS)
static struct zmk_widget_output_status output_status_widget;
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_LAYER_STATUS)
static struct zmk_widget_layer_status layer_status_widget;
#endif

#if IS_ENABLED(CONFIG_ZMK_WPM)
static lv_obj_t *wpm_label;

typedef struct {
    int wpm;
} custom_wpm_state_t;

static custom_wpm_state_t custom_wpm_get_state(const zmk_event_t *eh) {
    return (custom_wpm_state_t){.wpm = zmk_wpm_get_state()};
}

static void custom_wpm_update_cb(custom_wpm_state_t state) {
    if (wpm_label == NULL) {
        return;
    }
    char text[16];
    snprintf(text, sizeof(text), "%i WPM", state.wpm);
    lv_label_set_text(wpm_label, text);
    lv_obj_align(wpm_label, LV_ALIGN_BOTTOM_MID, 0, -1);
}

ZMK_DISPLAY_WIDGET_LISTENER(custom_wpm_widget, custom_wpm_state_t,
                            custom_wpm_update_cb, custom_wpm_get_state)
ZMK_SUBSCRIPTION(custom_wpm_widget, zmk_wpm_state_changed);
#endif /* IS_ENABLED(CONFIG_ZMK_WPM) */

#else
/* =========================================================================
 * Peripheral (Right) Half Widgets
 * ========================================================================= */

#if IS_ENABLED(CONFIG_ZMK_WIDGET_PERIPHERAL_STATUS)
static struct zmk_widget_peripheral_status peripheral_status_widget;
#endif

#define NUM_EQ_BARS 14
static lv_obj_t *eq_bars[NUM_EQ_BARS];
/* Perfect sine wave shape from 0 to 24 */
static const uint8_t wave_shape[14] = {12, 17, 21, 24, 24, 21, 17, 12, 6, 2, 0, 0, 2, 6};

static uint8_t anim_tick = 0;
static uint8_t anim_accum = 0;
static volatile uint8_t eq_momentum = 0;

static void eq_timer_cb(lv_timer_t *timer) {
    if (eq_momentum > 0) {
        /* Accumulate horizontal speed based on typing momentum */
        anim_accum += eq_momentum;
        while (anim_accum >= 30) {
            anim_tick++;
            anim_accum -= 30;
        }
        eq_momentum--; /* Decay amplitude and speed smoothly */
    }

    for (int i = 0; i < NUM_EQ_BARS; i++) {
        if (eq_bars[i] == NULL) continue;
        
        /* Get the wave's vertical value for this dot */
        uint8_t wave_val = wave_shape[(i + anim_tick) % 14];
        
        /* Scale the wave's amplitude mathematically by momentum (0 to 40) */
        /* Base height is 5. Wave adds up to 24 * (40/40) = 24 */
        uint8_t h = 5 + (wave_val * eq_momentum) / 40;

        /* 14 dots spaced across 128px */
        lv_obj_set_pos(eq_bars[i], 8 + (i * 8), 32 - h);
    }
}

#endif /* Central vs Peripheral */

/* Global key listener: accelerates EQ bars on typing */
static int key_press_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev != NULL && ev->state) {
#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        eq_momentum = 40; /* Boost animation momentum */
#endif
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(key_press_sub, key_press_listener);
ZMK_SUBSCRIPTION(key_press_sub, zmk_position_state_changed);

/* =========================================================================
 * Main Status Screen Entry Point
 * ========================================================================= */
lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_size(screen, 128, 32); /* Explicitly set size to prevent alignment bugs */
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
    /* Central (Left) Half - Outward BT, Centered Layer, Inward Battery */

    /* Line 1: Bluetooth / USB Output on Top-Left */
#if IS_ENABLED(CONFIG_ZMK_WIDGET_OUTPUT_STATUS)
    zmk_widget_output_status_init(&output_status_widget, screen);
    lv_obj_set_style_text_font(zmk_widget_output_status_obj(&output_status_widget),
                               lv_theme_get_font_small(screen), LV_PART_MAIN);
    lv_obj_align(zmk_widget_output_status_obj(&output_status_widget), LV_ALIGN_TOP_LEFT, 0, 0);
#endif

    /* Line 1: Active Layer Name on Top-Center */
#if IS_ENABLED(CONFIG_ZMK_WIDGET_LAYER_STATUS)
    zmk_widget_layer_status_init(&layer_status_widget, screen);
    lv_obj_set_style_text_font(zmk_widget_layer_status_obj(&layer_status_widget),
                               lv_theme_get_font_small(screen), LV_PART_MAIN);
    lv_obj_align(zmk_widget_layer_status_obj(&layer_status_widget), LV_ALIGN_TOP_MID, 0, 0);
#endif

    /* Line 1: Custom Battery Symbol on Top-Right */
    battery_label = lv_label_create(screen);
    lv_obj_set_style_text_font(battery_label, lv_theme_get_font_small(screen), LV_PART_MAIN);
    lv_label_set_text(battery_label, LV_SYMBOL_BATTERY_FULL);
    lv_obj_align(battery_label, LV_ALIGN_TOP_RIGHT, 0, 0);
    custom_battery_widget_init();

    /* Line 2: Large Centered WPM Display filling the bottom area */
#if IS_ENABLED(CONFIG_ZMK_WPM)
    wpm_label = lv_label_create(screen);
    lv_obj_clear_flag(wpm_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(wpm_label, lv_theme_get_font_large(screen), LV_PART_MAIN);
    lv_label_set_text(wpm_label, "0 WPM");
    lv_obj_align(wpm_label, LV_ALIGN_BOTTOM_MID, 0, -1);
    custom_wpm_widget_init();
#endif

#else
    /* Peripheral (Right) Half - Same side matching left */

    /* Line 1: Peripheral Connection Status on Top-Left */
#if IS_ENABLED(CONFIG_ZMK_WIDGET_PERIPHERAL_STATUS)
    zmk_widget_peripheral_status_init(&peripheral_status_widget, screen);
    lv_obj_set_style_text_font(zmk_widget_peripheral_status_obj(&peripheral_status_widget),
                               lv_theme_get_font_small(screen), LV_PART_MAIN);
    lv_obj_align(zmk_widget_peripheral_status_obj(&peripheral_status_widget), LV_ALIGN_TOP_LEFT, 0, 0);
#endif

    /* Line 1: Custom Battery Symbol on Top-Right */
    battery_label = lv_label_create(screen);
    lv_obj_set_style_text_font(battery_label, lv_theme_get_font_small(screen), LV_PART_MAIN);
    lv_label_set_text(battery_label, LV_SYMBOL_BATTERY_FULL);
    lv_obj_align(battery_label, LV_ALIGN_TOP_RIGHT, 0, 0);
    custom_battery_widget_init();

    /* Line 2: Bouncing Dot Visualizer using labels (Guaranteed rendering) */
    for (int i = 0; i < NUM_EQ_BARS; i++) {
        eq_bars[i] = lv_label_create(screen);
        lv_obj_set_style_text_font(eq_bars[i], lv_theme_get_font_small(screen), LV_PART_MAIN);
        lv_label_set_text(eq_bars[i], "O"); /* Use an 'O' character as a bouncing dot */
        
        /* Absolute positioning. X is fixed, Y starts at 27 (32 - 5) */
        lv_obj_set_pos(eq_bars[i], 8 + (i * 8), 27);
    }
    lv_timer_create(eq_timer_cb, 100, NULL);
#endif

    /* Shared Audio Pulse Boot Animation */
    start_intro_animation(screen);

    return screen;
}
