/*
 * Hardware layer: I2S audio output driving a MAX98357 Class-D DAC.
 * See audio.h.
 *
 * Wiring (from board_config.h):
 *   BCLK -> GPIO32, LRC(WS) -> GPIO15, DIN -> GPIO21, no MCLK.
 *
 * The I2S bus is configured as 16-bit STEREO; raw PCM is streamed for MP3
 * playback.
 *
 * Architecture: DIRECT decode-to-I2S path — no ring buffer, no feed task.
 * The MP3 player task (the only writer) applies the DSP chain and calls
 * i2s_channel_write() right here; the I2S DMA (12 x 1024 frames ~280 ms,
 * auto_clear) is the jitter buffer and paces the decoder by back-pressure,
 * so decode and hardware clock can never run away from each other.
 *
 * Channel lifecycle is a simple on/off:
 *   - parked (disabled) while idle or on the Bluetooth route: no BCLK/LRC
 *     is generated and the MAX98357 drops into its power-down state, so
 *     there is no idle current draw;
 *   - enabled by the first PCM write of a speaker session — AFTER the
 *     sample-rate reconfig, so the clock never starts ahead of the data;
 *   - parked again by hw_audio_set_player_active(false) or a route switch.
 * All channel operations are serialized by s_io_lock, and the IDF driver
 * itself releases an in-flight write when the channel is disabled
 * (i2s_common.c: i2s_channel_disable sets the state to READY and waits for
 * the write loop to exit), so stopping or re-routing from any task is safe.
 */
#define LOG_LOCAL_LEVEL ESP_LOG_INFO    /* keep detailed audio tracing out unless explicitly set to DEBUG at compile time */
#include "board_config.h"
#include "audio.h"
#include "bt_audio.h"

#include <math.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "hw_audio";

#define AUDIO_DEFAULT_RATE  44100
#define AUDIO_2PI           6.2831853f

/* I2S DMA ring: 12 descriptors x 1024 frames (x 4 bytes/frame, 16-bit
 * stereo) = 48 KB of buffering (~280 ms at 44.1 kHz). One descriptor-sized
 * zero chunk per descriptor is enough to silence the whole ring. */
#define I2S_DMA_DESC_NUM    12

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

static i2s_chan_handle_t s_tx;
static volatile bool s_ready;
static uint32_t s_rate = AUDIO_DEFAULT_RATE;
static volatile bool s_player_active;    /* MP3 player owns the I2S bus */

/* Active output route: a single, explicit either/or selection. Only
 * hw_audio_set_route() may change it; the writer streams to exactly this
 * destination and never probes the Bluetooth link itself. */
static audio_route_t s_route = AUDIO_ROUTE_SPEAKER;

/* Tracks whether the I2S channel is currently enabled (generating BCLK/LRC).
 * The channel is parked (i2s_channel_disable) whenever nothing is being
 * played or the audio is routed to Bluetooth, so the MAX98357 drops into
 * its power-down state — it shuts down ~64k BCLK cycles after the clock
 * stops. All reads/writes are serialized by s_io_lock. */
static bool s_i2s_enabled;

/* Serializes all channel operations (enable/disable/rate-reconfig/write).
 * The speaker path holds it around i2s_channel_write, so stop/park requests
 * from other tasks wait at most one bounded write instead of racing it. */
static SemaphoreHandle_t s_io_lock;

/* Consecutive failed I2S writes (bounds the WARN/ERROR rate). */
static uint32_t s_wr_errs;

/* Underflow diagnostics: the gap between consecutive speaker-path writes
 * (the decode time) must stay well under the DMA drain time (one 1024-frame
 * descriptor = ~23 ms at 44.1 kHz), otherwise the DMA runs dry between
 * frames and the amp reproduces repeated fragments / its noise floor.
 * s_last_write_us = end of the previous write (0 = parked since); a gap
 * > 30 ms is logged at WARN at most once per second. */
static int64_t s_last_write_us;
static int64_t s_last_gap_log_us;
static uint32_t s_gap_count;
static uint32_t s_enable_count;

/* Set true after hw_audio_write_pcm has triggered a channel rebuild for a
 * wedged DMA, so it will not rebuild again until the channel proves healthy
 * (a successful write, or a fresh enable) — bounds the recovery to one attempt
 * per channel session instead of spinning a rebuild every stalled frame. */
static bool s_rebuild_done;

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
    s_volume = audio_route_is_bt() ? s_vol_bt : s_vol_speaker;
    audio_update_vol_gain();
    audio_dsp_reset();
    s_vol_gain_sm = s_vol_gain;   /* no fade-in on route switch */
    if (audio_route_is_bt() && s_ready) {
        if (xSemaphoreTake(s_io_lock, pdMS_TO_TICKS(250)) == pdTRUE) {
            if (s_i2s_enabled) {
                if (i2s_channel_disable(s_tx) != ESP_OK) {
                    ESP_LOGW(TAG, "I2S park failed (route -> bt)");
                }
                s_i2s_enabled = false;
                s_last_write_us = 0;
            }
            xSemaphoreGive(s_io_lock);
        }
    }
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

/* Fill every DMA descriptor with silence while the channel is READY (parked)
 * or RUNNING. The descriptors' buffers are DRAM, so this works regardless of
 * channel state; the next enable clocks out silence instead of a previous
 * track's tail. Each descriptor holds 1024 frames x 4 bytes = 4096 bytes, so
 * one chunk per descriptor silences the whole ring. Caller holds s_io_lock
 * and s_tx != NULL. */
static void audio_silence_dma_ring(void)
{
    static const uint8_t s_zero_dma[4096] = {0};
    for (int i = 0; i < I2S_DMA_DESC_NUM; i++) {
        size_t loaded = 0;
        i2s_channel_preload_data(s_tx, s_zero_dma, sizeof(s_zero_dma), &loaded);
        if (loaded < sizeof(s_zero_dma)) {
            break;   /* ring full of silence */
        }
    }
}

/* Mark/unmark the MP3 player as the owner of the I2S bus. On release the
 * channel is parked immediately (BCLK stops, the MAX98357 powers down); on
 * claim only the flags/DSP are set up — the channel is actually enabled by
 * the first PCM write, so the clock never starts ahead of the data. */
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
        ESP_LOGI(TAG, "[PLAYER] audio pipeline ready");
    }
    else {
        /* Park the channel. Safe against a concurrent write: the lock
         * serializes with the in-flight i2s_channel_write, and the IDF
         * driver itself releases that write when the channel state flips
         * to READY (i2s_common.c i2s_channel_disable). */
        if (xSemaphoreTake(s_io_lock, pdMS_TO_TICKS(250)) == pdTRUE) {
            if (s_i2s_enabled) {
                if (i2s_channel_disable(s_tx) != ESP_OK) {
                    ESP_LOGW(TAG, "I2S park failed");
                }
                s_i2s_enabled = false;
                s_last_write_us = 0;
            }
            /* Clear the DMA ring's residual audio now. The descriptor buffers
             * still hold the PREVIOUS track's tail after a park, and the next
             * hw_audio_write_pcm re-enables the channel from this ring — so
             * without this the old tail clocks out first as "余音" on the next
             * track. Silencing leaves a clean, silent ring ready for re-enable.
             * Harmless on pause too (the channel is parked, so silence is what
             * the gap plays anyway). */
            if (s_tx != NULL) {
                audio_silence_dma_ring();
            }
            xSemaphoreGive(s_io_lock);
        }
        ESP_LOGD(TAG, "player inactive: I2S parked");
    }
}

bool hw_audio_is_ready(void)
{
    return s_ready && (s_tx != NULL) && (s_io_lock != NULL);
}

/* Create the I2S channel: std-mode init on the board pins. Leaves the
 * channel DISABLED (parked) so no BCLK is generated while idle (the MAX98357
 * powers down when its clock stops). The config validity is fully checked by
 * i2s_channel_init_std_mode() itself, so there is NO enable->disable probe
 * here: on ESP32 the out-link DMA start/stop is asynchronous (i2s_ll_tx_stop_link
 * sets out_link.stop with no completion wait, and i2s_ll_tx_reset's single-cycle
 * pulse may not latch while the module clock is off), and a boot-time stop-then-
 * restart can wedge the out-link FSM for the entire boot — the "sometimes no I2S
 * after power-on" failure. The runtime first write enables the channel instead.
 * Shared by boot init and the rebuild path. */
static esp_err_t audio_create_channel(void)
{
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = I2S_DMA_DESC_NUM,
        .dma_frame_num = 1024,
        .auto_clear = true,
    };
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S channel init failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_DEFAULT_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                     I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_NUM_I2S_BCLK,
            .ws   = PIN_NUM_I2S_LRC,
            .dout = PIN_NUM_I2S_DIN,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S std init failed: %s", esp_err_to_name(err));
        return err;
    }
    /* Channel left PARKED (no boot-time enable->disable probe; see the
     * function comment above for why). The first runtime write enables it. */
    s_i2s_enabled = false;
    return ESP_OK;
}

void hw_audio_init(void)
{
    s_io_lock = xSemaphoreCreateMutex();
    if (s_io_lock == NULL) {
        ESP_LOGE(TAG, "[AUDIO] io mutex create FAILED -> audio unusable");
        return;
    }

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

    ESP_LOGI(TAG, "[AUDIO] I2S ready (direct-write path, %u Hz default)",
             (unsigned)s_rate);

    /* Route starts at the speaker; nothing else may flip it (see hw_audio_set_route).
     * A Bluetooth link coming up does NOT hijack a speaker session — but a link
     * that drops must return immediately, regardless of which UI page is shown,
     * so a speaker session resumes without waiting for the user to poll. */
    s_route = AUDIO_ROUTE_SPEAKER;
    bt_audio_set_conn_state_cb(hw_audio_on_bt_conn_state);
}

/* Troubleshooting: rebuild the I2S channel from scratch (see audio.h).
 *
 * Safety: may be called while a track is playing. The only writer is the
 * decode task, and it serializes with this rebuild through s_io_lock — a
 * write in flight holds that lock, so taking it here also proves no write is
 * pending. The write path triggers this only AFTER a write has returned
 * STALLED, so the DMA is idle when we tear the channel down; the next write
 * re-enables the fresh channel at the current rate. A boot-time init failure
 * leaves s_tx NULL, and this path can still bring the channel up. */
esp_err_t hw_audio_rebuild_i2s(void)
{
    if (!s_ready || s_io_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Safe to run mid-playback: a write in flight holds s_io_lock, so taking it
     * here proves the DMA is idle; the caller (hw_audio_write_pcm) only
     * triggers a rebuild after a write has returned STALLED. */
    if (xSemaphoreTake(s_io_lock, pdMS_TO_TICKS(250)) != pdTRUE) {
        ESP_LOGW(TAG, "i2s lock busy, rebuild skipped");
        return ESP_ERR_TIMEOUT;
    }
    /* Park the old channel (if any) before tearing it down. A wedged DMA
     * surfaces as a timed-out write that already released the lock, so the
     * driver-side disable below is safe. */
    if (s_tx != NULL) {
        if (s_i2s_enabled) {
            if (i2s_channel_disable(s_tx) != ESP_OK) {
                ESP_LOGW(TAG, "I2S park failed before rebuild");
            }
            s_i2s_enabled = false;
            s_last_write_us = 0;
        }
        esp_err_t e = i2s_del_channel(s_tx);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "I2S channel delete failed: %s",
                     esp_err_to_name(e));
        }
        s_tx = NULL;
    }
    esp_err_t e = audio_create_channel();
    if (e != ESP_OK) {
        s_ready = false;              /* bus gone: block playback */
        xSemaphoreGive(s_io_lock);
        return e;
    }
    /* Recompute everything that depends on the (possibly changed) sample
     * rate and start the DSP history clean, settled at the current gains
     * (no fade-in on the first track after the rebuild). */
    audio_set_hpf_coeff(s_rate);
    audio_set_loudness_coeff(s_rate);
    audio_dsp_reset();
    s_vol_gain_sm = s_vol_gain;
    s_loud_boost_sm = s_loud_boost;
    s_track_gain_sm = s_track_gain;
    s_master_gain_sm = s_master_gain;
    s_wr_errs = 0;
    xSemaphoreGive(s_io_lock);
    ESP_LOGI(TAG, "I2S channel rebuilt (rate %u Hz)", (unsigned)s_rate);
    return ESP_OK;
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
    return s_player_active && s_i2s_enabled;
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

/* Reconfigure the I2S sample rate. Applied IMMEDIATELY from the calling task
 * (the MP3 player's decode task — the only user), so the new clock is in
 * place before the first PCM of a track reaches the DMA; there is no
 * deferred/pending rate that a pause/stop could lose. The driver requires
 * the READY (disabled) state for a reconfig, so a running channel is parked
 * first — the next write re-enables it at the new rate. */
void hw_audio_set_sample_rate(uint32_t sample_rate_hz)
{
    if (!s_ready || sample_rate_hz == 0 || sample_rate_hz == s_rate) {
        return;
    }
    if (xSemaphoreTake(s_io_lock, pdMS_TO_TICKS(250)) != pdTRUE) {
        ESP_LOGW(TAG, "i2s lock busy, rate change skipped (%u Hz)",
                 (unsigned)sample_rate_hz);
        return;
    }
    if (s_i2s_enabled) {
        if (i2s_channel_disable(s_tx) != ESP_OK) {
            ESP_LOGW(TAG, "I2S park failed before rate change");
        }
        s_i2s_enabled = false;
        s_last_write_us = 0;
    }
    /* Fill the DMA ring with silence while the channel is READY: the next
     * enable clocks out whatever the descriptors hold, and stale samples
     * recorded at the OLD rate would come out as a pitch-shifted squeal at
     * the new clock (this is the audible chirp at every track change). Each
     * descriptor holds 1024 frames x 4 bytes = 4096 bytes, so one chunk per
     * descriptor silences the whole ring. */
    audio_silence_dma_ring();
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
    esp_err_t e = i2s_channel_reconfig_std_clock(s_tx, &clk);
    xSemaphoreGive(s_io_lock);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "I2S rate reconfig to %u Hz failed: %s",
                 (unsigned)sample_rate_hz, esp_err_to_name(e));
        return;   /* keep the previous clock; playback may be off-pitch */
    }
    s_rate = sample_rate_hz;
    audio_set_hpf_coeff(s_rate);
    audio_set_loudness_coeff(s_rate);
    if (audio_route_is_bt()) {
        bt_audio_set_sample_rate(sample_rate_hz);
    }
    ESP_LOGI(TAG, "I2S rate -> %u Hz", (unsigned)s_rate);
}

/* Drop the previous pass's queued audio so a repeat-one replay starts clean.
 *
 * Bluetooth: the stale tail lives in the PCM ring, so really flush it.
 *
 * Speaker (I2S): the DMA ring IS the only buffer, and its descriptor QUEUE and
 * free-list are NOT reset by auto_clear (auto_clear only zeroes transmitted
 * data). A repeat-one seam that merely left the channel running therefore kept
 * the PREVIOUS pass's tail in the queue — and if that queue had desynced from
 * the writer (the ring drained, the ISR dropped entries), the next write would
 * clock stale audio or never see back-pressure and the pipeline-error detector
 * went blind. That is exactly the I2S-only failure (Bluetooth flushes its ring
 * here, so it never shows it). So for the speaker route we PARK the channel,
 * refill the whole ring with silence, and re-enable: the next write starts from
 * a clean, empty DMA. This is a single disable->enable while the channel is
 * RUNNING (not freshly inited), the same safe cycle hw_audio_set_sample_rate
 * uses — it cannot wedge the out-link the way the boot-time double toggle could.
 * The underrun bookkeeping is also reset so the deliberate seam pause is not
 * flagged as an I2S write gap. Call from the task that owns PCM writes. */
void hw_audio_pipeline_flush(void)
{
    s_last_write_us = 0;
    if (s_route == AUDIO_ROUTE_BT) {
        bt_audio_flush_pcm_ring();
        return;
    }
    /* Speaker route: clean-restart the DMA ring. */
    if (xSemaphoreTake(s_io_lock, pdMS_TO_TICKS(250)) == pdTRUE) {
        if (s_tx != NULL) {
            bool was_enabled = s_i2s_enabled;
            if (was_enabled) {
                if (i2s_channel_disable(s_tx) != ESP_OK) {
                    ESP_LOGW(TAG, "I2S park failed in pipeline flush");
                }
                s_i2s_enabled = false;
            }
            /* Fill every descriptor with silence so the re-enabled channel
             * clocks silence (not the previous pass's tail) until the next
             * write. */
            audio_silence_dma_ring();
            if (was_enabled) {
                esp_err_t e = i2s_channel_enable(s_tx);
                if (e != ESP_OK) {
                    ESP_LOGW(TAG, "I2S re-enable failed in pipeline flush: %s",
                             esp_err_to_name(e));
                } else {
                    s_i2s_enabled = true;
                    s_rebuild_done = false;   /* fresh channel session */
                    ESP_LOGI(TAG, "I2S flushed + re-enabled (session #%u)",
                             (unsigned)++s_enable_count);
                }
            }
        }
        xSemaphoreGive(s_io_lock);
    }
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
    size_t n = frames * 2;

    if (bt_out) {
        /* Park the speaker path while Bluetooth plays: no BCLK, amp off. */
        if (s_i2s_enabled) {
            if (xSemaphoreTake(s_io_lock, pdMS_TO_TICKS(250)) == pdTRUE) {
                if (s_i2s_enabled) {
                    i2s_channel_disable(s_tx);
                    s_i2s_enabled = false;
                }
                xSemaphoreGive(s_io_lock);
            }
        }
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
        int32_t x = stereo_frames[i];

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
        stereo_frames[i] = (int16_t)y;
    }
    s_vol_gain_sm = g;

    size_t bytes = frames * 4;
    /* Serialize with stop/park requests from other tasks: the lock is held
     * only around the driver calls, and a concurrent holder takes at most
     * one bounded 300 ms write, so 500 ms is ample. */
    if (xSemaphoreTake(s_io_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "i2s lock busy (timeout)");
        return AUDIO_WRITE_STALLED;
    }
    /* Underflow diagnostics: a write gap (decode time) well above one DMA
     * descriptor (~23 ms) means the DMA is draining dry between frames and
     * the amp is powered on near-silence — the "constant buzz" failure
     * mode. Logged at WARN, at most once per second, plus a lifetime count
     * so a session's totals can be read off at the end. */
    if (s_i2s_enabled) {
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
    /* Enable the channel only here, right before the data: the clock never
     * starts ahead of PCM (no initial auto-clear blank), and after a rate
     * reconfig the first write re-enables at the new rate. */
    if (!s_i2s_enabled) {
        esp_err_t e = i2s_channel_enable(s_tx);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "I2S enable failed: %s", esp_err_to_name(e));
            xSemaphoreGive(s_io_lock);
            return AUDIO_WRITE_STALLED;
        }
        s_i2s_enabled = true;
        s_rebuild_done = false;   /* fresh channel session: allow a rebuild */
        ESP_LOGI(TAG, "I2S enabled (session #%u)", (unsigned)++s_enable_count);
    }
    size_t w = 0;
    esp_err_t e = i2s_channel_write(s_tx, stereo_frames, bytes, &w,
                                    pdMS_TO_TICKS(300));
    s_last_write_us = esp_timer_get_time();
    xSemaphoreGive(s_io_lock);
    /* A stop/pause landed mid-write: the partial frame is not an error. */
    if (!s_player_active) {
        return AUDIO_WRITE_ABANDONED;
    }
    if (e != ESP_OK || w != bytes) {
        if (++s_wr_errs >= 4) {
            /* Classify the failure so the root cause is obvious from the log:
             * ESP_ERR_INVALID_STATE = the channel is not enabled (the binary
             * semaphore was never given: enable() failed, or the channel was
             * parked); ESP_ERR_TIMEOUT = the DMA is not consuming (the write
             * blocked the full 300 ms for a free descriptor). */
            const char *why = (e == ESP_ERR_INVALID_STATE)
                ? "channel not enabled (binary not given)"
                : (e == ESP_ERR_TIMEOUT)
                    ? "DMA not consuming (write timed out)"
                    : esp_err_to_name(e);
            ESP_LOGE(TAG, "[ERROR] I2S write failed: %s (%u/%u bytes)",
                     why, (unsigned)w, (unsigned)bytes);
            s_wr_errs = 0;
        }
        /* A wedged DMA (channel enabled but writes keep timing out / returning
         * zero bytes) cannot heal itself — the only recovery is a full channel
         * rebuild. Try it ONCE per channel session (guarded by s_rebuild_done):
         * tear the channel down and bring it back up; the next write re-enables
         * it at the current rate. If the rebuild does not restore DMA we keep
         * returning STALLED so the player aborts the track (and auto-advances)
         * cleanly instead of playing silence forever. */
        if (s_i2s_enabled && !s_rebuild_done) {
            s_rebuild_done = true;
            s_wr_errs = 0;
            ESP_LOGW(TAG, "I2S write wedged -> rebuilding channel");
            esp_err_t rb = hw_audio_rebuild_i2s();
            if (rb != ESP_OK) {
                ESP_LOGE(TAG, "I2S rebuild failed: %s", esp_err_to_name(rb));
            }
        }
        return AUDIO_WRITE_STALLED;
    }
    s_rebuild_done = false;   /* healthy write: a future wedge may rebuild */
    s_wr_errs = 0;
    return AUDIO_WRITE_OK;
}
