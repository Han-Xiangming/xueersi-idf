/*
 * Hardware layer: I2S audio output driving a MAX98357 mono Class-D DAC.
 *
 * Streams decoded MP3 PCM over I2S (BCLK/LRC/DIN); the MAX98357 derives its
 * own master clock from BCLK, so no MCLK is wired.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Initialize the I2S peripheral and the MAX98357 DAC. */
void hw_audio_init(void);

/* True once the I2S channel is up and ready to stream. */
bool hw_audio_ready(void);

/* Set/get output volume (0..100 %). */
void hw_audio_set_volume(uint8_t volume_pct);
uint8_t hw_audio_get_volume(void);

/* Reconfigure the I2S sample rate (e.g. to match an MP3 file's rate). */
void hw_audio_set_sample_rate(uint32_t sample_rate_hz);

/* Mark/unmark the MP3 player as the owner of the I2S bus. */
void hw_audio_set_player_active(bool active);

/* Stream raw 16-bit stereo PCM (L,R interleaved). `frames` = number of
 * L/R pairs. Used by the MP3 player to output decoded audio. Samples are
 * filtered in place by the speaker-protection high-pass before enqueueing. */
void hw_audio_write_pcm(int16_t *stereo_frames, size_t frames);
