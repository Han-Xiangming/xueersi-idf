/*
 * Hardware layer: I2S audio output driving a MAX98357 Class-D DAC.
 * See audio.h.
 *
 * Wiring (from board_config.h):
 *   BCLK -> GPIO32, LRC(WS) -> GPIO15, DIN -> GPIO21, no MCLK.
 *
 * The I2S bus is configured as 16-bit STEREO (fixed 44100 Hz) and driven by an
 * ADF i2s_stream WRITER element; raw PCM is streamed for MP3 playback.
 *
 * Architecture: DIRECT decode-to-I2S path with a SMALL ring buffer owned by the
 * i2s_stream element. The MP3 player task (the only writer) applies the DSP
 * chain and calls audio_element_write(); the i2s_stream writer task (its own
 * task) pops the ring and feeds the I2S DMA. The DMA is the jitter buffer and
 * paces the decoder by back-pressure, so decode and the hardware clock can
 * never run away from each other.
 *
 * The I2S clock is FIXED at init and is NEVER reconfigured for a sample-rate
 * change (decoded PCM is resampled to it in hw_audio_write_pcm instead). The
 * element is created once at init and left RUNNING; pause/resume — NOT
 * disable/enable — gates it (route switch to Bluetooth, end-of-playback park).
 * This removes the out-link stop/start cycle that wedged the ESP32 DMA.
 *
 * Idle output clocks digital silence through the running element; parking only
 * pauses the writer task to stop BCLK and power the MAX98357 down.
 */
#define LOG_LOCAL_LEVEL ESP_LOG_INFO    /* keep detailed audio tracing out unless explicitly set to DEBUG at compile time */
#include "board_config.h"
#include "audio.h"
#include "bt_audio.h"
#include "speex/speex_resampler.h"   /* OUTSIDE_SPEEX + FIXED_POINT: self-contained
                                     * fixed-point arbitrary-rate resampler */

#include <math.h>
#include <string.h>

#include "driver/i2s_std.h"   /* i2s_gpio_config_t / I2S_NUM_0 / I2S_ROLE_MASTER (供 i2s_stream gpio 配置) */
#include "audio_element.h"
#include "i2s_stream.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_attr.h"          /* EXT_RAM_BSS_ATTR for the resampler scratch */

static const char *TAG = "hw_audio";

#define AUDIO_DEFAULT_RATE  44100
#define AUDIO_2PI           6.2831853f

/* I2S DMA ring: 12 descriptors x 1024 frames (x 4 bytes/frame, 16-bit
 * stereo) = 48 KB of buffering (~280 ms at 44.1 kHz). One descriptor-sized
 * zero chunk per descriptor is enough to silence the whole ring. */
#define I2S_DMA_DESC_NUM    12

/* i2s_stream writer task owns the actual DMA write, so there is no manual
 * write timeout / IO mutex here: audio_element_write() blocks on the element's
 * ring buffer and the writer task feeds the DMA independently. */

/* Speaker-protection high-pass cutoff (Hz).
 *
 * Current speaker specs (racetrack phone unit):
 *   Resonance Fs  : 880 Hz (850 ~ 920 Hz table)
 *   Xmax          : ±0.22 mm (distortion limit)
 *   Frequency range: 800 Hz ~ 8000 Hz
 * The HPF removes everything below ~800 Hz so no excursion is wasted on
 * frequencies the cone cannot reproduce; with Xmax this small, raising the
 * corner from 700 Hz keeps the driver clear of its break-up region while
 * still passing the rated band. */
#define SPEAKER_HPF_FC_HZ   800

static audio_element_handle_t s_i2s_el = NULL;   /* i2s_stream WRITER 元素 */
static volatile bool s_ready;
static uint32_t s_rate = AUDIO_DEFAULT_RATE;

/* The I2S bus runs at ONE FIXED rate (s_rate) for the whole session: decoded
 * PCM is resampled to it in hw_audio_write_pcm, so a rate change NEVER
 * reconfigures the I2S clock. The i2s_stream element is created once at init
 * and left RUNNING; pause/resume (not disable/enable) gates it. This removes
 * the out-link stop/start cycle that wedged the ESP32 DMA ("plays at first,
 * then nothing, only over I2S"). See hw_audio_set_sample_rate(). */

/* SpeexDSP fixed-point resampler state (decoder rate -> fixed I2S rate).
 * Recreated per track (its history is the seam boundary), so no sample bleeds
 * across tracks. Under OUTSIDE_SPEEX + FIXED_POINT it is a self-contained,
 * arbitrary-rate resampler (cubic-interpolated sinc).
 *
 * QUALITY NOTE: quality scales the FIR filter length (and sinc-table size), so
 * it scales CPU ~linearly. On ESP32 (no FPU; fixed-point MACs) q5 processes one
 * 1152-sample stereo frame in ~22 ms — more than the 24 ms of audio it emits,
 * so the decode task becomes the bottleneck, the 279 ms DMA ring drains, and
 * "I2S write gap" underrun warnings appear. q2 (filt_len 16) is ~4x cheaper,
 * still a true windowed-sinc (vastly better than the old linear interpolator),
 * and leaves huge real-time slack; q1 (filt_len 8) is even faster if needed. */
#define RESAMP_QUALITY     2      /* 0..10; ESP32 sweet spot (fast + sinc quality) */
static SpeexResamplerState *s_resamp = NULL;  /* NULL => bypass */
static bool                s_resamp_active;   /* true: src_rate != s_rate */

/* Resampler output scratch (interleaved L/R int16). It is the DSP+DMA source,
 * so PSRAM is fine and keeps DRAM free. Sized for the largest realistic
 * upsample ratio (8 kHz -> 44.1 kHz ~5.5x) on a 1152-frame MP3 chunk, plus
 * headroom. */
#define RESAMP_MAX_FRAMES  7680
EXT_RAM_BSS_ATTR static int16_t s_rs_buf[RESAMP_MAX_FRAMES * 2];

/* Tear down the resampler and drop to bypass (frees DRAM, no resampling). */
static void resamp_free(void)
{
    if (s_resamp != NULL) {
        speex_resampler_destroy(s_resamp);
        s_resamp = NULL;
    }
    s_resamp_active = false;
}
static volatile bool s_player_active;    /* MP3 player owns the I2S bus */

/* Active output route: a single, explicit either/or selection. Only
 * hw_audio_set_route() may change it; the writer streams to exactly this
 * destination and never probes the Bluetooth link itself. */
static audio_route_t s_route = AUDIO_ROUTE_SPEAKER;

/* Consecutive failed i2s_stream writes (bounds the WARN/ERROR rate). */
static uint32_t s_wr_errs;

/* Underflow diagnostics: the gap between consecutive speaker-path writes
 * (the decode time) must stay well under the DMA drain time, otherwise the
 * DMA runs dry between frames and the amp reproduces repeated fragments /
 * its noise floor. s_last_write_us = end of the previous write (0 = idle
 * since); a gap > 30 ms is logged at WARN at most once per second. */
static int64_t s_last_write_us;
static int64_t s_last_gap_log_us;
static uint32_t s_gap_count;

/* --- Speaker-protection high-pass filter -------------------------------
 * The on-board driver is a small phone racetrack speaker (usable ~800 Hz..8 kHz,
 * Fs ≈ 880 Hz, Xmax ±0.22 mm). A 1st-order DC-blocking high-pass is inserted before
 * the DAC so DC offsets and deep sub-bass (which the cone cannot reproduce and
 * only waste excursion / power) are removed, without touching the voice band.
 *
 * Per-channel difference equation:
 *     y[n] = (x[n] - x[n-1]) + lambda * y[n-1]
 * where lambda is the recursive pole placed at the -3 dB cutoff f_c:
 *     lambda = cos(w) - sqrt((1 - cos(w)) * (3 - cos(w))),  w = 2*pi*f_c/f_s
 * lambda is stored in Q15 fixed point (computed only when the rate changes, so
 * no FPU is needed at runtime). */
static int32_t s_hpf_x1[2];              /* previous input, per channel */
static int32_t s_hpf_y1[2];              /* previous output, per channel */
static int32_t s_hpf_lambda;             /* Q15 recursive coefficient */

/* Recompute the Q15 high-pass coefficient for a new sample rate. */
static void audio_set_hpf_coeff(uint32_t rate)
{
    if (rate == 0) {
        return;
    }
    float w = AUDIO_2PI * (float)SPEAKER_HPF_FC_HZ / (float)rate;
    float c = cosf(w);
    float lambda = c - sqrtf((1.0f - c) * (3.0f - c));
    s_hpf_lambda = (int32_t)(lambda * 32768.0f);
    if (s_hpf_lambda < 1) {
        s_hpf_lambda = 1;                /* keep strictly stable */
    }
    else if (s_hpf_lambda > 32767) {
        s_hpf_lambda = 32767;
    }
    ESP_LOGD(TAG, "HPF coeff: rate=%u fc=%u lambda(Q15)=%d",
             (unsigned)rate, SPEAKER_HPF_FC_HZ, (int)s_hpf_lambda);
}

static uint8_t s_vol_speaker = 80;       /* per-route volume, percent */
static uint8_t s_vol_bt      = 80;
static uint8_t s_volume = 80;            /* active route volume (UI view) */
static int32_t s_vol_gain;               /* target Q15 linear gain */
static int32_t s_vol_gain_sm;            /* smoothed gain actually applied */

/* Perceptual volume taper (built once at init into s_vol_tab).
 *
 * A plain quadratic gain (v/100)² is concave in dB: near full scale a 5% step
 * is <1 dB, below the ear's just-noticeable-difference, so the top of the
 * range feels "stuck". We instead map percent linearly to attenuation in dB
 * (a true audio/log taper) so EVERY percent is the same number of dB and the
 * difference between adjacent settings is always clearly audible:
 *
 *     gain_dB(v) = (v/100 - 1) * VOL_MAX_ATTEN_DB      (0 dB at v=100)
 *     gain_lin   = 10^(gain_dB / 20)
 *     Q15        = gain_lin * 32767
 *
 *   vol   gain(Q15)  level(dB)   5% step (dB)
 *   100   32767        0.0        ~2.0 each (constant & obvious)
 *    70    8231      -12.0
 *    50    3277      -20.0
 *    30    1304      -28.0
 *    10     519      -36.0
 *     0       0        -∞          mute
 *
 * The table is built at 0.1 dB resolution (not per-percent) so the AVRCP
 * remote scale (0..127) also resolves to uniform ~0.32 dB steps. Mapping it
 * through the coarse percent grid instead made consecutive remote presses
 * alternate "no change / one step" and feel uneven. */
#define VOL_MAX_ATTEN_DB 40   /* total attenuation at v=1 (silence ≈ -40 dB);
                               * every 5% press ≈ 2 dB — clearly audible */
#define VOL_TAB_STEP_DB  0.1f /* table resolution: 0.1 dB per entry */
#define VOL_TAB_ENTRIES  ((VOL_MAX_ATTEN_DB * 10) + 1)  /* 40 dB / 0.1 dB + 0 dB */
static int32_t s_vol_tab[VOL_TAB_ENTRIES];

/* First-order gain smoothing (anti-zipper): s_vol_gain_sm chases s_vol_gain
 * with a ~5 ms time constant, so a volume change ramps between levels instead
 * of stepping — a mid-waveform gain jump lands as an audible click/pop. */
#define VOL_SMOOTH_A_Q15 148 /* alpha = 1 - e^(-1/(5 ms * 44.1 kHz)) in Q15 */

/* Loudness-compensation state (see the shelf section below). Declared before
 * audio_update_vol_gain(), which recomputes the boost target on volume
 * changes. */
#define LOUDNESS_MAX_DB        9.0f  /* boost at minimum volume */
static int32_t s_lp_coeff;            /* Q15 one-pole LP coefficient */
static int32_t s_lp_state[2];         /* per-channel LP history */
static int32_t s_loud_boost;          /* target boost, Q15 */
static int32_t s_loud_boost_sm;       /* smoothed boost, Q15 */

static int vol_tab_index(float dB)
{
    if (dB <= -(float)VOL_MAX_ATTEN_DB) {
        return 0;
    }
    if (dB >= 0.0f) {
        return VOL_TAB_ENTRIES - 1;
    }
    return (int)((dB + (float)VOL_MAX_ATTEN_DB) / VOL_TAB_STEP_DB + 0.5f);
}

static void audio_build_vol_table(void)
{
    for (int i = 0; i < VOL_TAB_ENTRIES; i++) {
        float dB = -(float)VOL_MAX_ATTEN_DB + i * VOL_TAB_STEP_DB;
        float g = powf(10.0f, dB / 20.0f);
        int32_t val = (int32_t)(g * 32767.0f + 0.5f);
        if (val > 32767) {
            val = 32767;
        }
        s_vol_tab[i] = val;
    }
}

/* Recompute the loudness bass-boost target from the current volume:
 * 0 dB at v=100, +LOUDNESS_MAX_DB at v=0 (see the shelf section below). */
static void audio_update_loudness_boost(void)
{
    int v = (int)s_volume;
    if (v < 0) {
        v = 0;
    }
    else if (v > 100) {
        v = 100;
    }
    float ldB = (1.0f - (float)v / 100.0f) * LOUDNESS_MAX_DB;
    s_loud_boost = (int32_t)((powf(10.0f, ldB / 20.0f) - 1.0f) * 32768.0f);
}

/* True while the active route is Bluetooth. Reads the single explicit route
 * state — never probes the Bluetooth link, so the decision is centralized and
 * stable for the whole write call. */
static bool audio_route_is_bt(void)
{
    return s_route == AUDIO_ROUTE_BT;
}

/* Forward declarations (defined later in this file). */
static void audio_update_vol_gain(void);
static void audio_dsp_reset(void);
static void resamp_free(void);
static void hw_audio_on_bt_conn_state(bool connected);

/* Apply a route change: switch the active volume slot to the new route and
 * reset the DSP filter history so the next stream starts clean. Switching
 * away from the speaker parks the channel (BCLK stops) so the MAX98357
 * powers down while Bluetooth owns the output. */
static void audio_apply_route(audio_route_t route)
{
    if (route == s_route) {
        return;
    }
    s_route = route;
    if (audio_route_is_bt()) {
        resamp_free();   /* speaker resampler not needed while BT owns output */
        /* 路由到 BT:暂停 i2s_stream writer → BCLK 停,MAX98357 休眠(省电)。
           扬声器路径不再写它,但元素保留,切回时 resume 即可。 */
        if (s_i2s_el != NULL) {
            audio_element_pause(s_i2s_el);
        }
    } else {
        /* 切回扬声器:恢复 writer(若被 pause)。 */
        if (s_i2s_el != NULL) {
            audio_element_resume(s_i2s_el);
        }
    }
    s_volume = audio_route_is_bt() ? s_vol_bt : s_vol_speaker;
    audio_update_vol_gain();
    audio_dsp_reset();
    s_vol_gain_sm = s_vol_gain;   /* no fade-in on route switch */
    s_last_write_us = 0;
    ESP_LOGI(TAG, "audio route -> %s (vol %u%%)",
             audio_route_is_bt() ? "bluetooth" : "speaker", (unsigned)s_volume);
}

void hw_audio_set_route(audio_route_t route)
{
    audio_apply_route(route);
}

audio_route_t hw_audio_get_route(void)
{
    return s_route;
}

static void audio_update_vol_gain(void)
{
    int v = (int)s_volume;
    if (v < 0) {
        v = 0;
    }
    else if (v > 100) {
        v = 100;
    }
    if (v == 0) {
        s_vol_gain = 0;                  /* hard mute */
        return;
    }
    float dB = ((float)v / 100.0f - 1.0f) * (float)VOL_MAX_ATTEN_DB;
    s_vol_gain = s_vol_tab[vol_tab_index(dB)];
    audio_update_loudness_boost();
}

/* --- Loudness compensation (volume-dependent bass shelf) ----------------
 * At low volumes, the ear is quieter to low frequencies, so quiet playback
 * sounds thin. A one-pole low-pass is run in parallel with the main path and
 * added back with a boost that grows as the volume falls:
 *
 *     y = x + boost(v) * lp(x),   boost(v) = 10^(dB/20) - 1
 *   dB = (1 - v/100) * LOUDNESS_MAX_DB      (0 dB at v=100, +9 dB at v=0)
 *
 * At full volume the boost is 0 dB, so the shelf only adds level where the
 * master gain is small; the soft limiter below still bounds the peaks.
 *
 * The corner sits at the rated band edge (800 Hz) of the racetrack unit so
 * the added signal stays inside what the cone can reproduce — a 250 Hz shelf
 * was inaudible on this driver (the HPF already cuts below 800 Hz). */
#define SPEAKER_LOUDNESS_FC_HZ 800   /* shelf low-pass corner (Hz) */

static void audio_set_loudness_coeff(uint32_t rate)
{
    if (rate == 0) {
        return;
    }
    float alpha = 1.0f - expf(-AUDIO_2PI * (float)SPEAKER_LOUDNESS_FC_HZ
                              / (float)rate);
    s_lp_coeff = (int32_t)(alpha * 32768.0f);
    if (s_lp_coeff < 1) {
        s_lp_coeff = 1;                /* keep strictly stable */
    }
    else if (s_lp_coeff > 32767) {
        s_lp_coeff = 32767;
    }
}

/* --- Soft limiter (anti-clipping / small-driver protection) -------------
 * A hot track at v=100 passes 0 dB straight to the DAC and clips. A peak
 * envelope (instant attack, ~100 ms release) drives a gain that drops fast
 * (~0.5 ms) above the threshold and recovers slowly, so transients are
 * handled without pumping. The gain curve is piecewise-linear, so no division
 * runs per sample: below the threshold the limiter is flat 0 dB, above it the
 * gain falls linearly to ~0.9 at full scale.
 *
 * The racetrack unit reaches its Xmax (±0.22 mm) early at high level, so the
 * threshold was pulled down from 30000 to curb the hot peaks sooner. */
#define LOUD_LIMIT_THRESH     27000  /* peaks above this get tamed (FS=32767) */
#define LOUD_LIMIT_SLOPE_Q15  18619  /* gain(env=32767) = 0.9 (-0.9 dB) */
#define LOUD_LIMIT_MIN_Q15    24576  /* gain floor (0.75), unreachable here */
#define LIM_ATT_Q15           1453   /* gain drop, ~0.5 ms @ 44.1 kHz */
#define LIM_REL_Q15           32753  /* envelope & gain release, ~100 ms */
static int32_t s_lim_env;             /* peak envelope */
static int32_t s_lim_gain;            /* smoothed limiter gain, Q15 */

/* --- Per-track loudness gain (ReplayGain) -------------------------------
 * Different sources are mastered at wildly different levels; a per-track
 * gain computed from the file's ReplayGain 2.0 tags (written by PC tools
 * like loudgain) brings them to a common loudness. The player sets it at
 * each track start via hw_audio_set_track_gain_db(); it is smoothed like
 * the volume, so a gain step at a track boundary never clicks. Applied
 * BEFORE the master volume on BOTH routes, so it scales the whole signal
 * the way a mastering gain would — the soft limiter below still caps the
 * peaks, which is exactly what ReplayGain assumes (limiting at 0 dBFS). */
#define RG_GAIN_MAX_DB 12.0f   /* clamp; loudgain values are normally ±6 dB */
static int32_t s_track_gain;    /* target Q15 per-track gain (32768 = 0 dB) */
static int32_t s_track_gain_sm; /* smoothed gain actually applied */

/* --- Master gain (user preamp) ------------------------------------------
 * A global -12..+12 dB gain applied to BOTH routes right after the per-track
 * ReplayGain and before the master volume (and, on the speaker route, before
 * the soft limiter, so hot peaks stay bounded). It shifts the whole signal
 * like a preamp knob: quiet sources can be pushed past what the volume knob
 * reaches at 100%, hot ones tamed. Persisted by the UI; smoothed like the
 * volume, so a change never clicks. */
#define MASTER_GAIN_MAX_DB 12.0f
static float    s_master_gain_db;   /* last set value, dB */
static int32_t  s_master_gain;      /* target Q15 gain (32768 = 0 dB) */
static int32_t  s_master_gain_sm;   /* smoothed gain actually applied */

/* Clear all per-track DSP history (call when a track starts or the route
 * switches). The limiter resets to flat gain so the first samples of a new
 * track are never ducked by the previous track's peak envelope. */
static void audio_dsp_reset(void)
{
    s_hpf_x1[0] = s_hpf_x1[1] = 0;
    s_hpf_y1[0] = s_hpf_y1[1] = 0;
    s_lp_state[0] = s_lp_state[1] = 0;
    s_lim_env = 0;
    s_lim_gain = 32768;
}

/* (The DMA-ring preload helper that used to live here is gone: it only existed
 * to hide a previous track's tail across a park, and the channel is no longer
 * parked at runtime — auto_clear zeroes each descriptor as it is transmitted,
 * so the ring erases its own tail. See the block comment below.) */

/* ---------------------------------------------------------------------------
 * WHY THE I2S CHANNEL IS NEVER STOPPED AT RUNTIME
 *
 * On ESP32 the I2S out-link stop is ASYNCHRONOUS: i2s_tx_channel_stop() (and
 * i2s_ll_tx_stop_link) sets out_link.stop with no completion wait, and the
 * next i2s_tx_channel_start() re-arms the link with i2s_hal_tx_reset() /
 * i2s_hal_tx_reset_dma(), whose single-cycle pulse may not latch while the
 * module clock is off. A stop-then-restart can therefore leave the out-link
 * FSM wedged — the DMA never runs again, so:
 *
 *   - no out-EOF interrupt ever fires,
 *   - i2s_channel_disable() resets the TX descriptor credit queue and
 *     i2s_channel_enable() does NOT refill it (only the RX path is reset),
 *   - so i2s_channel_write() blocks on xQueueReceive() for a credit that can
 *     never arrive, times out, and returns STALLED for the rest of the boot.
 *
 * That is exactly the reported failure: "sometimes fine after power-on, later
 * playback dead, log goes quiet, only over I2S". Bluetooth is never affected
 * because it never touches the I2S DMA (bt_audio_write_pcm() goes straight to
 * the A2DP ring).
 *
 * The fix is structural: enable the channel ONCE (on the first write) and
 * leave it RUNNING for the whole session. Idle output is handled by
 * auto_clear = true — i2s_dma_tx_callback() memsets each descriptor buffer
 * after it has been transmitted, so an un-fed ring clocks out true digital
 * silence (no buzz) and the previous track's tail is erased as the ring
 * drains, with no stop/start at all.
 *
 * Cost: BCLK keeps running while idle, so the MAX98357 stays out of shutdown
 * (a few mA). That is the deliberate trade for not gambling on the out-link.
 * The channel is still parked when the speaker is genuinely not in use for a
 * long time — i.e. when the route switches to Bluetooth.
 * ------------------------------------------------------------------------- */

/* Mark/unmark the MP3 player as the owner of the I2S bus. On claim only the
 * flags/DSP are set up — the channel is enabled by the first PCM write, so
 * the clock never starts ahead of the data. On release the channel is NOT
 * parked (see the block comment above): it keeps clocking auto_clear silence
 * so a later resume needs no out-link restart. */
void hw_audio_set_player_active(bool active)
{
    /* 原则3：audio 未 ready（I2S/互斥锁未建好）时禁止启动播放。 */
    if (active && !s_ready) {
        ESP_LOGE(TAG, "[ERROR] player active requested but audio not ready");
        return;
    }
    s_player_active = active;
    if (active) {
        audio_dsp_reset();            /* fresh filter history per track */
        s_vol_gain_sm = s_vol_gain;   /* start at full gain: no fade-in */
        audio_set_hpf_coeff(s_rate);  /* default-rate coeff until 1st frame */
        audio_set_loudness_coeff(s_rate);
        /* 恢复 writer(若被 park/路由切换暂停过)。元素在 init 时已 RUNNING,首次播放为无害 no-op。 */
        if (s_i2s_el != NULL) {
            audio_element_resume(s_i2s_el);
        }
        ESP_LOGI(TAG, "[PLAYER] audio pipeline ready");
    }
    else {
        /* Leave the channel RUNNING — see the block comment above this
         * function. Parking it here was the out-link stop/start that could
         * wedge the DMA for the rest of the boot.
         *
         * Nothing else is needed for a clean stop: s_player_active = false
         * makes every hw_audio_write_pcm() return immediately, and auto_clear
         * zeroes each descriptor as it is transmitted, so the queued tail is
         * replaced by silence and the ring drains by itself. That is also what
         * removes the "余音" of the previous track — by the time the next track
         * starts (a button press later) the ring is silent.
         *
         * The underrun bookkeeping is reset so the deliberate idle gap is not
         * counted as a write gap. */
        s_last_write_us = 0;
        ESP_LOGD(TAG, "player inactive: I2S left running (auto_clear silence)");
    }
}

/* Really stop playback: pause the i2s_stream writer task. This stops BCLK and
 * powers the MAX98357 down — but does NOT disable/rebuild the I2S channel, so
 * there is no out-link stop/start cycle that could wedge the ESP32 DMA. The
 * element is resumed by the next hw_audio_set_player_active(true) (or by a
 * route switch back to the speaker).
 *
 * Call this ONLY when audio is genuinely finished — when the decode loop exits
 * (stop / watchdog / give-up). It is deliberately NOT called on pause or
 * between tracks. */
void hw_audio_park(void)
{
    /* 暂停 writer:BCLK 停,MAX98357 休眠。Ring 中残留由 writer 在 pause 前自然
       排空;下次播放 resume 即干净。不再 disable/重建 I2S。 */
    if (s_i2s_el != NULL) {
        audio_element_pause(s_i2s_el);
    }
    s_last_write_us = 0;
    ESP_LOGD(TAG, "I2S parked (writer paused)");
}

bool hw_audio_is_ready(void)
{
    return s_ready && (s_i2s_el != NULL);
}

/* Create the I2S output as a standalone ADF i2s_stream WRITER element.
 * Codec-less: no audio_board_init / set_codec (the MAX98357 is a pure-I2S
 * Class-D DAC). The element is created at ONE FIXED rate (s_rate) and left
 * RUNNING; pause/resume gates it — there is no runtime enable/disable/reconfig
 * that could wedge the ESP32 out-link. The board pins are carried over from the
 * old std-mode config. NOTE: the gpio_cfg / field names below follow a recent
 * esp-adf-libs layout; adapt to your ADF version if it differs. */
static esp_err_t audio_create_channel(void)
{
    i2s_stream_cfg_t cfg = I2S_STREAM_CFG_DEFAULT();
    cfg.type = AUDIO_STREAM_WRITER;
    cfg.i2s_config.sample_rate = (int)s_rate;       /* 固定 I2S 时钟,绝不随轨道变 */
    cfg.i2s_config.bits         = 16;
    cfg.i2s_config.channels     = 2;
    cfg.i2s_config.i2s_port     = I2S_NUM_0;
    cfg.i2s_config.chan_cfg.role        = I2S_ROLE_MASTER;
    cfg.i2s_config.chan_cfg.dma_desc_num  = I2S_DMA_DESC_NUM;
    cfg.i2s_config.chan_cfg.dma_frame_num = 1024;
    cfg.out_rb_size = 8 * 1024;                     /* ringbuf,吸收 decode 抖动 */
    /* 引脚:沿用原 std 配置(mclk 未用,MAX98357 从 BCLK 派生主时钟)。 */
    cfg.i2s_config.gpio_cfg = (i2s_gpio_config_t){
        .mclk = I2S_GPIO_UNUSED,
        .bclk = PIN_NUM_I2S_BCLK,
        .ws   = PIN_NUM_I2S_LRC,
        .dout = PIN_NUM_I2S_DIN,
        .din  = I2S_GPIO_UNUSED,
    };
    s_i2s_el = i2s_stream_init(&cfg);
    if (s_i2s_el == NULL) {
        ESP_LOGE(TAG, "[AUDIO] i2s_stream init failed");
        return ESP_FAIL;
    }
    esp_err_t e = audio_element_run(s_i2s_el);   /* 启动 writer 任务(此时 enable I2S) */
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "[AUDIO] i2s_stream run failed: %s", esp_err_to_name(e));
        audio_element_deinit(s_i2s_el);
        s_i2s_el = NULL;
        return e;
    }
    return ESP_OK;
}

void hw_audio_init(void)
{
    if (audio_create_channel() != ESP_OK) {
        return;
    }

    audio_set_hpf_coeff(s_rate);      /* default-rate HPF coefficient */
    audio_set_loudness_coeff(s_rate); /* default-rate loudness shelf coeff */
    audio_build_vol_table();          /* precompute the 0.1 dB gain table */
    audio_update_vol_gain();
    s_vol_gain_sm = s_vol_gain;       /* start settled: no fade-in from zero */
    s_loud_boost_sm = s_loud_boost;   /* shelf boost settled too */
    s_track_gain = s_track_gain_sm = 32768;   /* 0 dB per-track gain */
    s_master_gain = s_master_gain_sm = 32768; /* 0 dB master gain, settled */
    s_lim_gain = 32768;               /* limiter flat until audio starts */

    s_ready = true;

    ESP_LOGI(TAG, "[AUDIO] I2S ready (i2s_stream WRITER, %u Hz fixed)",
             (unsigned)s_rate);

    /* Route starts at the speaker; nothing else may flip it (see hw_audio_set_route).
     * A Bluetooth link coming up does NOT hijack a speaker session — but a link
     * that drops must return immediately, regardless of which UI page is shown,
     * so a speaker session resumes without waiting for the user to poll. */
    s_route = AUDIO_ROUTE_SPEAKER;
    bt_audio_set_conn_state_cb(hw_audio_on_bt_conn_state);
}


/* Bluetooth link callback. On a drop (remote power-off / out of range / failed
 * dial-out) we return the route to the speaker at once. On connect we do NOT
 * auto-take the route — that stays an explicit user action in the BT page, so a
 * speaker session is never silently hijacked. */
static void hw_audio_on_bt_conn_state(bool connected)
{
    if (!connected) {
        audio_apply_route(AUDIO_ROUTE_SPEAKER);
    }
}

void hw_audio_set_volume(uint8_t volume_pct)
{
    if (volume_pct > 100) {
        volume_pct = 100;
    }
    if (audio_route_is_bt()) {
        s_vol_bt = volume_pct;
    }
    else {
        s_vol_speaker = volume_pct;
    }
    s_volume = volume_pct;
    audio_update_vol_gain();
}

uint8_t hw_audio_get_volume(void)
{
    return s_volume;
}

bool hw_audio_is_playing(void)
{
    return s_player_active && (s_i2s_el != NULL);
}

void hw_audio_set_speaker_volume(uint8_t volume_pct)
{
    if (volume_pct > 100) {
        volume_pct = 100;
    }
    s_vol_speaker = volume_pct;
    if (!audio_route_is_bt()) {
        s_volume = volume_pct;
        audio_update_vol_gain();
    }
}

uint8_t hw_audio_get_speaker_volume(void)
{
    return s_vol_speaker;
}

void hw_audio_set_bt_volume(uint8_t volume_pct)
{
    if (volume_pct > 100) {
        volume_pct = 100;
    }
    s_vol_bt = volume_pct;
    if (audio_route_is_bt()) {
        s_volume = volume_pct;
        audio_update_vol_gain();
    }
}

uint8_t hw_audio_get_bt_volume(void)
{
    return s_vol_bt;
}

/* AVRCP absolute volume (0..127, full remote scale): each remote step is an
 * equal ~0.32 dB (40/127) change, resolved through the same 0.1 dB gain table
 * as the local percent volume, so the remote and the buttons share one
 * consistent taper. Writes the BT route's slot (the remote is a Bluetooth
 * peer) and updates the percent view (UI/NVS). */
void hw_audio_set_avrc_volume(uint8_t volume_0_127)
{
    if (volume_0_127 > 127) {
        volume_0_127 = 127;
    }
    s_vol_bt = (uint8_t)(((uint32_t)volume_0_127 * 100u + 63) / 127u);
    s_volume = s_vol_bt;
    if (volume_0_127 == 0) {
        s_vol_gain = 0;                /* hard mute */
        audio_update_loudness_boost();
        return;
    }
    float dB = ((float)volume_0_127 / 127.0f - 1.0f) * (float)VOL_MAX_ATTEN_DB;
    s_vol_gain = s_vol_tab[vol_tab_index(dB)];
    audio_update_loudness_boost();
}

/* Per-track loudness gain (ReplayGain), dB. 0 = flat; clamped to ±12 dB and
 * converted to a Q15 gain the DSP chain applies before the master volume.
 * Called by the player at each track start (0 dB when the file is
 * untagged). The smoothing ramps to it over ~5 ms, so no click at the
 * boundary. */
void hw_audio_set_track_gain_db(float gain_db)
{
    if (gain_db > RG_GAIN_MAX_DB) {
        gain_db = RG_GAIN_MAX_DB;
    }
    else if (gain_db < -RG_GAIN_MAX_DB) {
        gain_db = -RG_GAIN_MAX_DB;
    }
    s_track_gain = (int32_t)(powf(10.0f, gain_db / 20.0f) * 32768.0f + 0.5f);
    if (gain_db != 0.0f) {
        ESP_LOGI(TAG, "track gain %.2f dB (Q15=%d)", gain_db,
                 (int)s_track_gain);
    }
}

/* Global master gain (user preamp), dB. Clamped to ±12 dB and converted to a
 * Q15 gain the DSP chain applies right after the per-track ReplayGain, on
 * both routes. The smoothing ramps to it over ~5 ms, so a change never
 * clicks. Set by the UI (settings page) at any time, even mid-playback. */
void hw_audio_set_master_gain_db(float gain_db)
{
    if (gain_db > MASTER_GAIN_MAX_DB) {
        gain_db = MASTER_GAIN_MAX_DB;
    }
    else if (gain_db < -MASTER_GAIN_MAX_DB) {
        gain_db = -MASTER_GAIN_MAX_DB;
    }
    s_master_gain_db = gain_db;
    s_master_gain = (int32_t)(powf(10.0f, gain_db / 20.0f) * 32768.0f + 0.5f);
    ESP_LOGI(TAG, "master gain %+.1f dB (Q15=%d)", gain_db, (int)s_master_gain);
}

float hw_audio_get_master_gain_db(void)
{
    return s_master_gain_db;
}

/* Declare the DECODER's native sample rate for the current track.
 *
 * The I2S channel runs at ONE FIXED rate (s_rate, set once at init) for the
 * whole session. Decoded PCM is resampled to that rate inside
 * hw_audio_write_pcm, so a rate change NEVER rebuilds/disables the channel —
 * which removes the in-playback out-link stop/start that wedged the ESP32 DMA
 * ("plays at first, then nothing, only over I2S" on the second track of a
 * mixed-rate playlist; BT was unaffected because it bypasses I2S entirely).
 *
 * This is the fix for the random-loop / single-loop silence: those modes pick
 * arbitrary or repeat files whose rate differs from the previous track, which
 * used to tear the channel down at the seam. With a fixed I2S rate the
 * channel is enabled once and never torn down between tracks, so the DMA cannot
 * wedge. Correct pitch is preserved by the resampler. */
void hw_audio_set_sample_rate(uint32_t sample_rate_hz)
{
    if (sample_rate_hz == 0) {
        return;
    }
    if (audio_route_is_bt()) {
        /* BT takes the original PCM; the SBC encoder handles its own rate.
         * No resampling, no I2S channel involved. */
        bt_audio_set_sample_rate(sample_rate_hz);
        return;
    }
    /* Speaker route: arm the resampler (or bypass when the decoder rate
     * already matches the fixed I2S rate). The DSP coefficients stay at the
     * fixed I2S rate (s_rate) because all PCM is resampled to it before the
     * DSP chain runs. The resampler is recreated per track call, which also
     * resets its history at the seam so no sample bleeds across tracks. */
    if (sample_rate_hz == s_rate) {
        resamp_free();             /* 1:1: bypass, zero overhead */
        return;
    }
    /* (Re)create the SpeexDSP resampler for this track's rate -> fixed I2S rate.
     * It is a self-contained fixed-point arbitrary-rate resampler, so no custom
     * (buggy) ratio math is needed. */
    resamp_free();
    int err = 0;
    s_resamp = speex_resampler_init(2, sample_rate_hz, s_rate,
                                    RESAMP_QUALITY, &err);
    if (err != 0 || s_resamp == NULL) {
        ESP_LOGE(TAG, "speex resampler init failed (rate %u, err %d)",
                 (unsigned)sample_rate_hz, err);
        s_resamp = NULL;
        s_resamp_active = false;   /* fallback: bypass (wrong pitch, no crash) */
        return;
    }
    speex_resampler_skip_zeros(s_resamp);   /* drop startup latency padding */
    s_resamp_active = true;
    ESP_LOGI(TAG, "resampler armed: %u -> %u Hz (speex q=%d)",
             (unsigned)sample_rate_hz, (unsigned)s_rate, RESAMP_QUALITY);
}

/* Drop the previous pass's queued audio so a repeat-one replay starts clean.
 *
 * Bluetooth: the stale tail lives in the PCM ring, so really flush it.
 *
 * Speaker (I2S): NOTHING is done to the channel. This used to park it, refill
 * the whole DMA ring with silence and re-enable it — a full out-link
 * stop/start on EVERY repeat-one seam, which is precisely the cycle that can
 * wedge the ESP32 out-link for the rest of the boot (see the block comment
 * above hw_audio_set_player_active). It was also unnecessary: auto_clear
 * zeroes each descriptor once the DMA has transmitted it, so as the ring
 * drains the old pass's tail is replaced by silence on its own, and the new
 * pass is written straight behind it. No stop/start, no chirp, no wedge.
 *
 * The underrun bookkeeping is reset so the deliberate seam pause is not
 * flagged as an I2S write gap. Call from the task that owns PCM writes. */
void hw_audio_pipeline_flush(void)
{
    s_last_write_us = 0;
    if (s_route == AUDIO_ROUTE_BT) {
        bt_audio_flush_pcm_ring();
        return;
    }
    /* Speaker route: the channel keeps running; auto_clear drains it to
     * silence. Deliberately no i2s_channel_disable()/enable() here. */
}

/* Resample `pairs_in` stereo L/R pairs (decoder native rate) to the fixed I2S
 * rate via SpeexDSP (2-channel interleaved int16). State persists inside the
 * SpeexResamplerState across calls, so the resampled stream is continuous
 * across MP3 frames; the resampler is recreated per track to reset that history
 * at the seam. Returns the number of output pairs written into `out` (at most
 * RESAMP_MAX_FRAMES). Caller guarantees pairs_in > 0 and s_resamp != NULL. */
static size_t resamp_process_pairs(const int16_t *in, size_t pairs_in,
                                   int16_t *out)
{
    const spx_int16_t *src = (const spx_int16_t *)in;
    spx_int16_t *dst = (spx_int16_t *)out;
    spx_uint32_t produced = 0;
    spx_uint32_t consumed = 0;

    /* Feed the whole frame; SpeexDSP buffers internally and emits as much output
     * as fits. Loop only if the output scratch filled before all input was
     * consumed (won't happen for our 8k..48k / 1152-sample frames). */
    while (consumed < (spx_uint32_t)pairs_in) {
        spx_uint32_t in_arg  = (spx_uint32_t)pairs_in - consumed;
        spx_uint32_t out_arg = RESAMP_MAX_FRAMES - produced;
        if (out_arg == 0) {
            break;   /* defensive: output scratch exhausted */
        }
        int err = speex_resampler_process_interleaved_int(
            s_resamp, src + consumed * 2, &in_arg,
            dst + produced * 2, &out_arg);
        consumed += in_arg;
        produced += out_arg;
        if (err != 0 || in_arg == 0) {
            break;   /* error or no progress -> stop */
        }
    }
    if (consumed < (spx_uint32_t)pairs_in) {
        ESP_LOGW(TAG, "resampler: dropped %u input pairs (scratch full)",
                 (unsigned)((spx_uint32_t)pairs_in - consumed));
    }
    return (size_t)produced;
}

/* Stream decoded 16-bit stereo PCM (L,R interleaved). `frames` = number of
 * L/R pairs. Returns the write result so the player can tell a wedged
 * pipeline (AUDIO_WRITE_STALLED) from a clean pause/stop
 * (AUDIO_WRITE_ABANDONED) and recover instead of playing silence forever.
 *
 * Output routing (like a phone): while a Bluetooth sink is linked, audio goes
 * to the headphones ONLY — full band (no speaker high-pass, headphones can
 * reproduce bass) with master volume applied, and the I2S channel stays
 * parked so the amp is powered down. The A2DP data callback is the ONE and
 * only pacer of the decode task (via the blocking send into the BT ring).
 *
 * Without Bluetooth, the speaker path applies the protection high-pass,
 * the loudness bass shelf, the per-track ReplayGain, the user master gain,
 * the master volume and the soft limiter. The Bluetooth path applies the
 * per-track gain, the user master gain and the master volume only
 * (headphones reproduce full band, and the sink's own
 * limiting handles hot peaks).
 *
 * Speaker output goes DIRECTLY to the I2S DMA (no ring buffer): the bounded
 * write blocks under DMA back-pressure, which paces the decoder at exactly
 * the hardware clock rate, and the DMA (12 x 1024 frames, auto_clear) absorbs
 * decode jitter. A wedged DMA surfaces as AUDIO_WRITE_STALLED; a stop/pause
 * mid-write surfaces as AUDIO_WRITE_ABANDONED. */
audio_write_result_t hw_audio_write_pcm(int16_t *stereo_frames, size_t frames)
{
    if (!s_ready || !s_player_active || frames == 0) {
        return AUDIO_WRITE_ABANDONED;
    }

    /* Route is a single explicit decision held in s_route (set only via
     * hw_audio_set_route). We never probe the Bluetooth link here; audio goes
     * to exactly one destination for this whole call. */
    const bool bt_out = (s_route == AUDIO_ROUTE_BT);

    const int32_t g_target = s_vol_gain;    /* Q15 target logarithmic gain */
    int32_t g = s_vol_gain_sm;              /* smoothed gain to apply */

    /* Speaker path: resample the decoder's native rate to the fixed I2S rate
     * before DSP. BT takes the original PCM (SBC encodes its own rate), so it
     * is skipped here. dsp_buf/dsp_frames are what the DSP chain + DMA run on. */
    int16_t *dsp_buf = stereo_frames;
    size_t   dsp_frames = frames;
    if (!bt_out && s_resamp_active && s_resamp != NULL) {
        dsp_frames = resamp_process_pairs(stereo_frames, frames, s_rs_buf);
        dsp_buf = s_rs_buf;
    }
    size_t n = dsp_frames * 2;

    if (bt_out) {
        /* 路由到 BT 时,扬声器 i2s_stream 元素已在 set_route 中 pause(BCLK 停),
           这里只做 BT 发送与增益。 */
        /* Bluetooth route: volume only, full band. The blocking send inside
         * bt_audio_write_pcm() paces the decoder. The send is now BOUNDED
         * (~2 s): a stalled BT sink surfaces as AUDIO_WRITE_STALLED here,
         * which the player counts and turns into a visible pipeline error —
         * the decode task is never left blocked inside this call forever. */
        for (size_t i = 0; i < n; i++) {
            /* Per-track ReplayGain first, then the user master gain (both
             * clamped to 16-bit: the BT path has no limiter to catch
             * over-boosted peaks). */
            s_track_gain_sm += ((s_track_gain - s_track_gain_sm) * VOL_SMOOTH_A_Q15) >> 15;
            int32_t t = (int32_t)(((int64_t)stereo_frames[i] * s_track_gain_sm) >> 15);
            if (t > 32767) {
                t = 32767;
            }
            else if (t < -32768) {
                t = -32768;
            }
            s_master_gain_sm += ((s_master_gain - s_master_gain_sm) * VOL_SMOOTH_A_Q15) >> 15;
            t = (int32_t)(((int64_t)t * s_master_gain_sm) >> 15);
            if (t > 32767) {
                t = 32767;
            }
            else if (t < -32768) {
                t = -32768;
            }
            g += ((g_target - g) * VOL_SMOOTH_A_Q15) >> 15;
            stereo_frames[i] = (int16_t)((t * g) >> 15);
        }
        s_vol_gain_sm = g;
        bool bt_ok = bt_audio_write_pcm(stereo_frames, frames);
        return bt_ok ? AUDIO_WRITE_OK : AUDIO_WRITE_STALLED;
    }

    /* Speaker route: high-pass -> loudness bass shelf -> per-track gain
     * -> user master gain -> master volume -> soft limiter, all per sample
     * (L = even index, R = odd index). */
    for (size_t i = 0; i < n; i++) {
        int ch = (int)(i & 1);
        int32_t x = dsp_buf[i];

        /* Speaker-protection high-pass */
        int32_t y = (x - s_hpf_x1[ch]) + ((s_hpf_lambda * s_hpf_y1[ch]) >> 15);
        s_hpf_x1[ch] = x;
        s_hpf_y1[ch] = y;
        if (y > 32767) {
            y = 32767;
        }
        else if (y < -32768) {
            y = -32768;
        }

        /* Loudness: add back the volume-dependent bass shelf */
        int32_t lp = s_lp_state[ch]
                     + (((y - s_lp_state[ch]) * s_lp_coeff) >> 15);
        s_lp_state[ch] = lp;
        s_loud_boost_sm += ((s_loud_boost - s_loud_boost_sm) * VOL_SMOOTH_A_Q15) >> 15;
        y = y + ((s_loud_boost_sm * lp) >> 15);
        if (y > 32767) {
            y = 32767;
        }
        else if (y < -32768) {
            y = -32768;
        }

        /* Per-track ReplayGain (pre-master-gain, so it scales the whole
         * signal; the limiter below still bounds the peaks). 64-bit
         * multiply: the gain can exceed 1.0 (up to +12 dB). */
        s_track_gain_sm += ((s_track_gain - s_track_gain_sm) * VOL_SMOOTH_A_Q15) >> 15;
        y = (int32_t)(((int64_t)y * s_track_gain_sm) >> 15);
        if (y > 32767) {
            y = 32767;
        }
        else if (y < -32768) {
            y = -32768;
        }

        /* User master gain (preamp offset, pre-volume; the soft limiter
         * below still bounds the peaks). Same 64-bit multiply: ±12 dB can
         * scale by up to ~4x. */
        s_master_gain_sm += ((s_master_gain - s_master_gain_sm) * VOL_SMOOTH_A_Q15) >> 15;
        y = (int32_t)(((int64_t)y * s_master_gain_sm) >> 15);
        if (y > 32767) {
            y = 32767;
        }
        else if (y < -32768) {
            y = -32768;
        }

        g += ((g_target - g) * VOL_SMOOTH_A_Q15) >> 15;
        y = (y * g) >> 15;               /* apply logarithmic volume */

        /* Soft limiter: tame peaks above the threshold */
        int32_t a = (y < 0) ? -y : y;
        if (a > s_lim_env) {
            s_lim_env = a;               /* instant peak attack */
        }
        else {
            s_lim_env = (s_lim_env * LIM_REL_Q15) >> 15;   /* slow release */
        }
        int32_t tg = 32768;              /* flat 0 dB below threshold */
        if (s_lim_env > LOUD_LIMIT_THRESH) {
            tg = 32768 - (((s_lim_env - LOUD_LIMIT_THRESH) * LOUD_LIMIT_SLOPE_Q15) >> 15);
            if (tg < LOUD_LIMIT_MIN_Q15) {
                tg = LOUD_LIMIT_MIN_Q15;
            }
        }
        if (tg < s_lim_gain) {
            s_lim_gain += ((tg - s_lim_gain) * LIM_ATT_Q15) >> 15;
        }
        else {
            s_lim_gain += ((tg - s_lim_gain) * LIM_REL_Q15) >> 15;
        }
        y = (y * s_lim_gain) >> 15;
        dsp_buf[i] = (int16_t)y;
    }
    s_vol_gain_sm = g;

    size_t bytes = dsp_frames * 4;

    /* Underflow diagnostics: the gap between consecutive speaker-path writes
     * (the decode time) must stay well under the DMA drain time, otherwise the
     * DMA runs dry between frames. s_last_write_us = end of previous write
     * (0 = idle since); a gap > 30 ms is logged at WARN at most once/sec. */
    if (s_i2s_el != NULL) {
        int64_t now_us = esp_timer_get_time();
        if (s_last_write_us != 0) {
            int64_t gap_ms = (now_us - s_last_write_us) / 1000;
            if (gap_ms > 30) {
                s_gap_count++;
                if (now_us - s_last_gap_log_us > 1000000) {
                    ESP_LOGW(TAG, "I2S write gap %lld ms (DMA underrun risk), "
                                  "total gaps %u",
                             (long long)gap_ms, (unsigned)s_gap_count);
                    s_last_gap_log_us = now_us;
                }
            }
        }
    }

    /* 元素已在 init 时 enable+run;这里只把已重采样+DSP 后的 PCM 写入它的
       ringbuf,由 i2s_stream 自己的 writer 任务喂 DMA。无 enable/预热/reconfig/
       互斥锁。backpressure 由 ringbuf 自然处理。 */
    if (s_i2s_el == NULL) {
        return AUDIO_WRITE_STALLED;
    }
    int wrote = audio_element_write(s_i2s_el, (char *)dsp_buf, (int)bytes);
    s_last_write_us = esp_timer_get_time();
    /* 暂停/停止落在写中途:不算错误。 */
    if (!s_player_active) {
        return AUDIO_WRITE_ABANDONED;
    }
    if (wrote < (int)bytes) {
        if (++s_wr_errs >= 4) {
            ESP_LOGE(TAG, "[ERROR] i2s_stream write failed (%d/%d bytes)",
                     wrote, (int)bytes);
            s_wr_errs = 0;
        }
        return AUDIO_WRITE_STALLED;
    }
    s_wr_errs = 0;
    return AUDIO_WRITE_OK;
}
