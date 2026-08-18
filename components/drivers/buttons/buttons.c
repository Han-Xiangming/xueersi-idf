/*
 * Hardware layer: key button input.
 * See buttons.h.
 */
#include "board_config.h"
#include "buttons.h"

#include "driver/gpio.h"
#include "lvgl.h"

typedef struct {
    gpio_num_t gpio;
    uint32_t key;
    const char *name;
    uint32_t active_level;   /* 1 = high-active, 0 = low-active */
} board_button_t;

/* All keys are low-active (press pulls the pin to GND with internal pull-up). */
static const board_button_t s_buttons[] = {
    {GPIO_NUM_2, LV_KEY_UP, "UP", 0},
    {GPIO_NUM_13, LV_KEY_DOWN, "DOWN", 0},
    {GPIO_NUM_27, LV_KEY_LEFT, "LEFT", 0},
    {GPIO_NUM_35, LV_KEY_RIGHT, "RIGHT", 0},
    {GPIO_NUM_34, LV_KEY_ENTER, "A", 0},
    {GPIO_NUM_12, LV_KEY_ESC, "B", 0},
    {GPIO_NUM_25, LV_KEY_HOME, "SELECT", 0},   /* Select */
    {GPIO_NUM_26, LV_KEY_END, "START", 0},    /* Start: wake only, no function */
    {GPIO_NUM_33, LV_KEY_MEDIA_PANEL, "MENU", 0},  /* Menu: playback panel */
};

void hw_buttons_init(void)
{
    uint64_t pin_mask = 0;
    uint64_t pullup_mask = 0;
    uint64_t pulldown_mask = 0;

    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        pin_mask |= 1ULL << s_buttons[i].gpio;
        if (s_buttons[i].active_level == 0) {
            /* low-active: need internal pull-up (except true open-drain ADC pins) */
            if (s_buttons[i].gpio != GPIO_NUM_34 && s_buttons[i].gpio != GPIO_NUM_35) {
                pullup_mask |= 1ULL << s_buttons[i].gpio;
            }
        } else {
            /* high-active: need internal pull-down */
            pulldown_mask |= 1ULL << s_buttons[i].gpio;
        }
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    if (pullup_mask) {
        gpio_config_t pullup_conf = {
            .pin_bit_mask = pullup_mask,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&pullup_conf));
    }

    if (pulldown_mask) {
        gpio_config_t pulldown_conf = {
            .pin_bit_mask = pulldown_mask,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&pulldown_conf));
    }
}

/* Media keys are delivered single-shot: report the press only on the poll
 * where the debounce settles, then look released while the button is still
 * held. This keeps LVGL's keypad state machine out of long-press
 * auto-repeat (which re-sends the same key every 90 ms while held), so one
 * START/MENU press performs exactly one action. */
static bool key_is_media(uint32_t key)
{
    return key == LV_KEY_MEDIA_PANEL;
}

void hw_buttons_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    static int last_raw_index = -1;
    static int stable_index = -1;
    static uint32_t raw_changed_ms = 0;
    static uint32_t last_key = LV_KEY_ENTER;
    static int media_hold_index = -1;

    int raw_index = -1;
    const uint32_t now_ms = lv_tick_get();

    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        if (gpio_get_level(s_buttons[i].gpio) == s_buttons[i].active_level) {
            raw_index = (int)i;
            break;
        }
    }

    if (raw_index != last_raw_index) {
        last_raw_index = raw_index;
        raw_changed_ms = now_ms;
        if (raw_index < 0) {
            stable_index = -1;
        }
    }
    if (lv_tick_elaps(raw_changed_ms) >= BUTTON_DEBOUNCE_MS) {
        stable_index = last_raw_index;
    }

    if (stable_index >= 0) {
        const uint32_t key = s_buttons[stable_index].key;
        if (key_is_media(key)) {
            if (stable_index != media_hold_index) {
                media_hold_index = stable_index;   /* first settling poll: press */
                last_key = key;
                data->state = LV_INDEV_STATE_PRESSED;
                data->key = key;
            }
            else {
                data->state = LV_INDEV_STATE_RELEASED;  /* still held: release */
                data->key = last_key;
            }
            return;
        }
        media_hold_index = -1;
        last_key = key;
        data->state = LV_INDEV_STATE_PRESSED;
        data->key = last_key;
    }
    else {
        media_hold_index = -1;
        data->state = LV_INDEV_STATE_RELEASED;
        data->key = last_key;
    }
}
