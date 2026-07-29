/*
 * Hardware layer: passive buzzer driven by LEDC PWM on GPIO14.
 *
 * The driver owns its own ready/stop state; the software layer only
 * requests beeps and polling of the internal stop timer.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

void hw_buzzer_init(void);
void hw_buzzer_beep(uint32_t freq_hz, uint32_t ms);
void hw_buzzer_stop(void);
bool hw_buzzer_ready(void);

/* Call periodically from the main loop to honor timed beep stops. */
void hw_buzzer_process_timers(void);
