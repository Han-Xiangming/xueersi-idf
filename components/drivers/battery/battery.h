/*
 * Hardware layer: single-cell Li-ion battery level sensing.
 *
 * Reads the pack voltage through a 2-resistor divider on GPIO 39 (ADC1_CH3)
 * and converts it to a percentage via a piecewise open-circuit voltage table
 * (a Li-ion cell's discharge curve is highly non-linear — a linear map puts
 * the 50 % point at the wrong voltage and makes the gauge wander).
 *
 * See PIN_NUM_BAT_ADC / BAT_DIV_FACTOR in board_config.h for the wiring.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Invoked on a low-battery crossing. `pct` is the current level; `recovered`
 * is false when the level just dropped to/below the threshold (act to protect
 * the cell) and true when it climbed back above the threshold + hysteresis
 * margin (restore any user setting the handler changed). */
typedef void (*bt_battery_low_cb_t)(uint8_t pct, bool recovered);

/* Initialise the ADC1 oneshot unit and the battery sampler.
 * Safe no-op if battery sensing is not built in. Call once at boot. */
void hw_battery_init(void);

/* Latest measured battery voltage in volts (post-divider scaling).
 * Before the first sample completes it returns 0.0f. */
float hw_battery_voltage(void);

/* Latest battery level as an integer 0..100 %.
 * Returns 0 before the first sample; 100 is clamped at BAT_V_FULL. */
uint8_t hw_battery_percent(void);

/* Low-battery guard. Fires `cb` once each time the level crosses DOWN through
 * `threshold_pct` (and re-arms only after the level climbs back above it by the
 * hysteresis margin), so the application can pause playback / dim the backlight
 * before the cell collapses and risks corrupting the SD card's FAT. The handler
 * runs from the battery sample timer context. Pass NULL to disable. */
void hw_battery_set_low_warn(uint8_t threshold_pct, bt_battery_low_cb_t cb);

/* Kick a single foreground sample and update the cached values.
 * The periodic task calls this internally; exposed so the UI can force a
 * refresh on demand (e.g. when the settings page is opened). */
void hw_battery_sample(void);
