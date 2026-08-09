/*
 * Hardware layer: key button input.
 * See hardware/buttons.h.
 */
#include "board_config.h"
#include "buttons.h"

#include "driver/gpio.h"
#include "lvgl.h"

typedef struct {
    gpio_num_t gpio;
    uint32_t key;
    const char *name;
} board_button_t;

static const board_button_t s_buttons[] = {
    {GPIO_NUM_2, LV_KEY_UP, "UP"},
    {GPIO_NUM_13, LV_KEY_DOWN, "DOWN"},
    {GPIO_NUM_27, LV_KEY_LEFT, "LEFT"},
    {GPIO_NUM_35, LV_KEY_RIGHT, "RIGHT"},
    {GPIO_NUM_25, LV_KEY_ENTER, "A"},
    {GPIO_NUM_26, LV_KEY_ESC, "B"},
    // {GPIO_NUM_34, LV_KEY_ENTER, "A"},
    // {GPIO_NUM_12, LV_KEY_ESC, "B"},
};

void hw_buttons_init(void)
{
    uint64_t pin_mask = 0;
    uint64_t pullup_mask = 0;

    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        pin_mask |= 1ULL << s_buttons[i].gpio;
        if (s_buttons[i].gpio != GPIO_NUM_34 && s_buttons[i].gpio != GPIO_NUM_35) {
            pullup_mask |= 1ULL << s_buttons[i].gpio;
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

    gpio_config_t pullup_conf = {
        .pin_bit_mask = pullup_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pullup_conf));
}

void hw_buttons_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    static int last_raw_index = -1;
    static int stable_index = -1;
    static uint32_t raw_changed_ms = 0;
    static uint32_t last_key = LV_KEY_ENTER;

    int raw_index = -1;
    const uint32_t now_ms = lv_tick_get();

    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        if (gpio_get_level(s_buttons[i].gpio) == BUTTON_ACTIVE_LEVEL) {
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
        last_key = s_buttons[stable_index].key;
        data->state = LV_INDEV_STATE_PRESSED;
        data->key = last_key;
    }
    else {
        data->state = LV_INDEV_STATE_RELEASED;
        data->key = last_key;
    }
}
