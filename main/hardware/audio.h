/*
 * Hardware layer: I2S audio output driving a MAX98357 mono Class-D DAC.
 *
 * The driver generates a sine tone and streams it over I2S (BCLK/LRC/DIN);
 * the MAX98357 derives its own master clock from BCLK, so no MCLK is wired.
 * Tone playback is non-blocking: start a tone, then pump samples from the
 * main loop via hw_audio_process_timers().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

/* Initialize the I2S peripheral and the MAX98357 DAC. */
void hw_audio_init(void);

/* True once the I2S channel is up and ready to stream. */
bool hw_audio_ready(void);

/* Start a sine tone of `freq_hz` for `duration_ms` (non-blocking). */
void hw_audio_tone(uint32_t freq_hz, uint32_t duration_ms);

/* Set/get output volume (0..100 %). Affects tone amplitude. */
void hw_audio_set_volume(uint8_t volume_pct);
uint8_t hw_audio_get_volume(void);

/* Enable/disable all audio output (sound-effects master switch). */
void hw_audio_set_enabled(bool enabled);
bool hw_audio_is_enabled(void);

/* Call periodically from the main loop to stream and stop timed tones. */
void hw_audio_process_timers(void);
