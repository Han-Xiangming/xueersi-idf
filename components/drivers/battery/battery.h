/*
 * Hardware layer: single-cell Li-ion battery level sensing.
 *
 * Reads the pack voltage through a 2-resistor divider on GPIO 39 (ADC1_CH3)
 * and converts it to a percentage via a simple linear Li-ion curve.
 *
 * See PIN_NUM_BAT_ADC / BAT_DIV_FACTOR in board_config.h for the wiring.
 *
 * See PIN_NUM_BAT_ADC / BAT_DIV_FACTOR in board_config.h for the wiring.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Initialise the ADC1 oneshot unit and the battery sampler.
 * Safe no-op if battery sensing is not built in. Call once at boot. */
void hw_battery_init(void);

/* Latest measured battery voltage in volts (post-divider scaling).
 * Before the first sample completes it returns 0.0f. */
float hw_battery_voltage(void);

/* Latest battery level as an integer 0..100 %.
 * Returns 0 before the first sample; 100 is clamped at BAT_V_FULL. */
uint8_t hw_battery_percent(void);

/* Kick a single foreground sample and update the cached values.
 * The periodic task calls this internally; exposed so the UI can force a
 * refresh on demand (e.g. when the settings page is opened). */
void hw_battery_sample(void);
