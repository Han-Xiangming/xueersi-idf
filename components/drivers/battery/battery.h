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
 * refresh on demand (e.g. when the settings page is opened). A forced sample
 * bypasses the "freeze while playing" guard, so the gauge still refreshes
 * when the user opens the settings page even mid-playback. */
void hw_battery_sample(void);

/* --- I2S load compensation ----------------------------------------------
 * While the speaker plays, the pack sags a few tens of mV under load, which
 * makes the open-circuit (rested) lookup table read low. Two mitigations:
 *
 *  1) Load compensation: add a small offset to the measured pack voltage that
 *     scales with the current output volume (proxy for load current). Set the
 *     per-100%-volume slope in mV via hw_battery_set_load_comp_mv(). The
 *     actual offset applied is slope_mv * volume_pct / 100.
 *
 *  2) Freeze while playing: the periodic sampler skips updating the cached
 *     voltage/percent while audio is actively clocking to the DAC, so the
 *     gauge holds its last (rested) value instead of drifting down under load.
 *     Forced samples (settings page) still refresh. Toggle with
 *     hw_battery_set_freeze_while_playing(). */
void hw_battery_set_load_comp_mv(uint16_t comp_mv_per_100pct);
uint16_t hw_battery_get_load_comp_mv(void);
void hw_battery_set_freeze_while_playing(bool enable);
bool hw_battery_get_freeze_while_playing(void);
