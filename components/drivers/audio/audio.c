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
 * MP3 playback uses a decoupled pipeline: the decoder task enqueues decoded
 * PCM frames into a ring buffer (with back-pressure so no samples are ever
 * dropped), and a dedicated high-priority feed task streams them to the I2S
 * DMA continuously. This keeps the DMA fed even when a complex MP3 frame
 * takes longer to decode, which previously caused I2S underruns / crackle.
 * The feed task also parks the I2S channel (stops BCLK/LRC) whenever nothing
 * is being played or the audio is routed to Bluetooth, so the MAX98357
 * powers down instead of drawing current on an idle clock.
 *
 * To keep long sessions free of dropouts the feed task aligns its clock to
 * the decoder's: it watches the ring-fill slope and inserts an occasional
 * duplicated sample at the I2S write stage, cancelling the ppm-level mismatch
 * between the MP3 sample rate and the APLL-derived BCLK without any
 * resampling (see the "Playback-clock alignment" block below). The feed is
 * event-driven (task-notified by the writer) instead of polling, and sample
 * rate reconfigs never start the clock ahead of the data (see apply_rate()).
 */
#define LOG_LOCAL_LEVEL ESP_LOG_INFO    /* keep detailed audio tracing out unless explicitly set to DEBUG at compile time */
#include "board_config.h"
#include "audio.h"
#include "bt_audio.h"

#include <math.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

static const char *TAG = "hw_audio";

#define AUDIO_DEFAULT_RATE  44100
#define AUDIO_2PI           6.2831853f

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

/* PCM ring buffer: decouples MP3 decode from I2S output. Sized for >1s of
 * 44.1kHz stereo 16-bit audio so decode jitter / SD stalls are absorbed. */
#define PCM_RING_BYTES      (256 * 1024)

static i2s_chan_handle_t s_tx;
static volatile bool s_ready;
static uint32_t s_rate = AUDIO_DEFAULT_RATE;
static volatile bool s_player_active;    /* MP3 player owns the I2S bus */

/* Active output route: a single, explicit either/or selection. Only
 * hw_audio_set_route() may change it; the writer streams to exactly this
 * destination and never probes the Bluetooth link itself. */
static audio_route_t s_route = AUDIO_ROUTE_SPEAKER;

/* Tracks whether the I2S channel is currently enabled (generating BCLK/LRC).
 * The feed task parks the channel (i2s_channel_disable) whenever nothing is
 * being played, so the MAX98357 drops into its power-down state — it shuts
 * down ~64k BCLK cycles after the clock stops. Only touched from the feed
 * task (and once at init), so no locking is needed. */
static bool s_i2s_enabled;

/* PCM ring buffer + feed task. */
static uint8_t *s_ring_storage;
static StaticRingbuffer_t s_ring_struct;
static RingbufHandle_t s_pcm_ring;
static TaskHandle_t s_feed_task = NULL;
static volatile bool s_feeding;
static uint32_t s_pending_rate;          /* applied serially by the feed task */

/* --- Playback-clock alignment -------------------------------------------
 * The decoder produces PCM at the exact sample rate claimed by the MP3
 * header, but the I2S clock (APLL-derived BCLK) is only ppm-accurate. The
 * 256 KB ring absorbs that drift for a long while, but over a long session
 * the fill walks in one direction until it drains — and an underrun is an
 * audible blank. (A fill-up is self-limiting: the xRingbufferSend
 * back-pressure just paces the decoder down, which is inaudible.)
 *
 * Fix: once a second the feed task measures the smoothed ring-fill slope.
 * While the fill is decaying it "inserts" an occasional duplicate L/R pair
 * at the I2S write stage — a repeated ~23 µs segment, inaudible at the
 * 1..4 pairs/s this corrects (±22..90 ppm) — cancelling the sink's clock
 * debt 1:1. All state is feed-task-private, so no locking is needed. */
#define FILL_SLOPE_TICK_MS 1000                  /* slope measurement period */
#define FILL_SLOPE_FILTER  0.4f                  /* ~2 s smoothing of the slope */
#define CORR_MAX_Q16       ((int32_t)(4.0f * 65536.0f / 44100.0f)) /* ~4 p/s cap */

static int64_t  s_fill_last_ms;        /* last slope tick (ms), 0 = unprimed */
static int32_t  s_fill_prev;           /* ring fill at last tick (bytes) */
static float    s_fill_slope;          /* smoothed fill slope, pairs/s (+) */
static int32_t  s_corr_rate_q16;       /* insert probability, Q16 per pair */
static int32_t  s_corr_acc_q16;        /* fractional insert accumulator */
static int16_t  s_last_pair[2];        /* last L/R pair written to I2S */
static uint32_t s_starve;              /* I2S blanks while a stream ran */
static uint32_t s_corr_ins;            /* inserted pair count (debug) */
static bool     s_session_wrote;       /* any data written this session */

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
 * reset the DSP filter history so the next stream starts clean. */
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

/* Apply a sample-rate change. Must only be called from the feed task so it is
 * serialized with the I2S writes (reconfig disables the channel). The channel
 * is left DISABLED here on purpose: the speaker path re-enables it in the
 * same loop pass, right before the receive/write, so the BCLK never starts
 * ahead of the data in the ring (no initial auto-clear blank, no spurious
 * enable/disable churn when the channel was already parked). When the
 * channel is parked (idle / BT route) the reconfig runs straight on the
 * disabled channel. */
static void apply_rate(uint32_t rate)
{
    if (!s_ready || rate == s_rate) {
        return;
    }
    ESP_LOGI(TAG, "reconfig I2S rate %u -> %u", (unsigned)s_rate, (unsigned)rate);
    if (s_i2s_enabled) {
        i2s_channel_disable(s_tx);
        s_i2s_enabled = false;
    }
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    i2s_channel_reconfig_std_clock(s_tx, &clk);
    s_rate = rate;
    audio_set_hpf_coeff(s_rate);
    audio_set_loudness_coeff(s_rate);
}

/* Playback-clock alignment, once per second while a speaker stream flows
 * (see the state block at the top). Called from the feed task only. */
static void audio_clock_tick(void)
{
    if (s_pcm_ring == NULL) {
        return;
    }
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_fill_last_ms == 0) {
        /* Session start (or BT pause): prime the baseline from the current
         * ring level, no correction until a full period has passed. */
        s_fill_last_ms = now_ms;
        s_fill_prev = (int32_t)(PCM_RING_BYTES
                                - (int32_t)xRingbufferGetCurFreeSize(s_pcm_ring));
        return;
    }
    if (now_ms - s_fill_last_ms < FILL_SLOPE_TICK_MS) {
        return;
    }
    const int32_t fill = (int32_t)(PCM_RING_BYTES
                                   - (int32_t)xRingbufferGetCurFreeSize(s_pcm_ring));
    const float dt_ms = (float)(now_ms - s_fill_last_ms);
    /* Ring-fill slope in sample PAIRS per second (+ = filling). */
    const float slope = (float)(fill - s_fill_prev) / (4.0f * dt_ms / 1000.0f);
    s_fill_slope += (slope - s_fill_slope) * FILL_SLOPE_FILTER;
    s_fill_prev = fill;
    s_fill_last_ms = now_ms;

    /* Only the drain direction is correctable: a filling ring is already
     * back-pressure limited, so nothing audible ever happens there. Cancel
     * the measured drain 1:1, capped at ~90 ppm. */
    float corr = (s_fill_slope < 0.0f) ? -s_fill_slope : 0.0f;
    int32_t cq = (int32_t)(corr * (65536.0f / 44100.0f) + 0.5f);
    s_corr_rate_q16 = (cq > CORR_MAX_Q16) ? CORR_MAX_Q16 : cq;

    if (s_starve > 0) {
        ESP_LOGW(TAG, "clock: %u I2S starves (auto-clear blanks), corr=%d p/s, inserts=%u",
                 (unsigned)s_starve,
                 (int)(((int64_t)s_corr_rate_q16 * 44100 + 32768) / 65536),
                 (unsigned)s_corr_ins);
        s_starve = 0;
        s_corr_ins = 0;
    }
}

/* Per-pair correction accounting for one chunk just handed to the I2S DMA.
 * `pairs` = number of L/R pairs in the chunk. While the ring-fill drain debt
 * exists (s_corr_rate_q16 > 0), every pair written earns a fractional chance
 * to insert a duplicate of the last pair — so inserts land ~randomly spread
 * across the stream instead of as salvoes. Called from the feed task only. */
static void audio_clock_insert_pairs(const int16_t *data, size_t pairs)
{
    for (size_t k = 0; k < pairs; k++) {
        s_last_pair[0] = data[2 * k];
        s_last_pair[1] = data[2 * k + 1];
        s_corr_acc_q16 += s_corr_rate_q16;
        if (s_corr_acc_q16 < 65536) {
            continue;
        }
        s_corr_acc_q16 -= 65536;
        size_t w2 = 0;
        if (i2s_channel_write(s_tx, (const uint8_t *)s_last_pair, 4, &w2,
                              pdMS_TO_TICKS(10)) == ESP_OK && w2 == 4) {
            s_corr_ins++;
        }
    }
}

static void audio_feed_task(void *arg)
{
    (void)arg;
    uint8_t *item;
    size_t item_size;

    for (;;) {
        /* Wait for play start. The while() also covers the case where a new
         * play notification was consumed during the park grace window below. */
        while (!s_feeding) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
        while (s_feeding) {
            /* Bluetooth route: decoded audio goes to the sink, the I2S ring is
             * never fed. Park the channel so the amp does not burn power on an
             * idle BCLK while the headphones play. */
            if (audio_route_is_bt()) {
                if (s_ready && s_i2s_enabled) {
                    i2s_channel_disable(s_tx);
                    s_i2s_enabled = false;
                }
                s_fill_last_ms = 0;   /* speaker clock state goes stale: re-prime */
                item = (uint8_t *)xRingbufferReceive(s_pcm_ring, &item_size,
                                                     pdMS_TO_TICKS(50));
                if (item != NULL) {
                    vRingbufferReturnItem(s_pcm_ring, item);
                }
                continue;
            }
            if (s_pending_rate != 0) {
                apply_rate(s_pending_rate);
                s_pending_rate = 0;
            }
            /* Speaker route: bring the channel back up if it is parked.
             * apply_rate() leaves the channel disabled, so it is always
             * started HERE, right before the receive/write — the clock never
             * runs ahead of the data in the ring. */
            if (s_ready && !s_i2s_enabled) {
                esp_err_t e = i2s_channel_enable(s_tx);
                if (e != ESP_OK) {
                    ESP_LOGW(TAG, "I2S enable failed: %s", esp_err_to_name(e));
                }
                else {
                    s_i2s_enabled = true;
                }
            }
            /* Playback-clock alignment pass (see the state block above). */
            audio_clock_tick();

            /* Event-driven receive: woken instantly by the ring send itself,
             * or by the writer/control notifies (xRingbufferReceive does NOT
             * see task notifies — hence the explicit wait below). The 100 ms
             * timeout is a safety net so stop/park requests are never missed
             * while the ring sits empty. */
            for (;;) {
                item = (uint8_t *)xRingbufferReceive(s_pcm_ring, &item_size, 0);
                if (item != NULL || !s_feeding) {
                    break;
                }
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            }
            if (item == NULL) {
                if (s_session_wrote && s_ready && s_i2s_enabled) {
                    s_starve++;   /* DMA blanked while a stream was running */
                }
                continue;   /* ring empty: DMA underruns (auto_clear -> silence) */
            }
            s_session_wrote = true;
            size_t written = 0;
            while (written < item_size) {
                size_t w = 0;
                esp_err_t e = i2s_channel_write(s_tx, item + written,
                                               item_size - written, &w,
                                               pdMS_TO_TICKS(100));
                if (e != ESP_OK || w == 0) {
                    break;
                }
                written += w;
                audio_clock_insert_pairs((const int16_t *)(item + written - w),
                                         w / 4);
            }
            vRingbufferReturnItem(s_pcm_ring, item);
        }
        /* Drain the PCM ring without writing to I2S — this makes stop/pause
         * instantaneous rather than playing out the remaining ~1.5 s buffer. */
        while ((item = (uint8_t *)xRingbufferReceive(s_pcm_ring, &item_size, 0)) != NULL) {
            vRingbufferReturnItem(s_pcm_ring, item);
        }
        /* Session over: reset the clock-alignment baseline so the next
         * session re-primes from the current ring level. */
        s_fill_last_ms = 0;
        s_session_wrote = false;
        s_corr_rate_q16 = 0;
        s_corr_acc_q16 = 0;
        /* 3 s park grace window: keep BCLK running so a quick re-play
         * (next/prev track, pause-resume) does not power-cycle the MAX98357
         * (which clicks) and does not restart the I2S clock. A play within
         * the window consumes the notification and re-enters the feed loop
         * with the channel still enabled; only after the window expires the
         * channel is parked and the amp drops to its idle off state. */
        while (s_ready && s_i2s_enabled && !s_feeding) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));
        }
        if (s_ready && s_i2s_enabled && !s_feeding) {
            i2s_channel_disable(s_tx);
            s_i2s_enabled = false;
        }
    }
}

void hw_audio_init(void)
{
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 8,
        .dma_frame_num = 1024,
        .auto_clear = true,
    };
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S channel init failed: %s", esp_err_to_name(err));
        return;
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
        return;
    }
    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S channel enable failed: %s", esp_err_to_name(err));
        return;
    }
    /* Park the channel right after init: the feed task re-enables it when a
     * track actually starts, so no BCLK is generated while the device idles
     * at the menu (the MAX98357 powers down when its clock stops). */
    i2s_channel_disable(s_tx);
    s_i2s_enabled = false;
    s_ready = true;
    /* Debug tracing is compiled in (LOG_LOCAL_LEVEL) but off by default;
     * the settings page LOG option enables it at runtime. */

    /* PCM ring buffer (external PSRAM when available, else internal heap). */
    s_ring_storage = heap_caps_malloc(PCM_RING_BYTES, MALLOC_CAP_SPIRAM);
    if (s_ring_storage == NULL) {
        s_ring_storage = malloc(PCM_RING_BYTES);
    }
    if (s_ring_storage != NULL) {
        s_pcm_ring = xRingbufferCreateStatic(PCM_RING_BYTES,
                                            RINGBUF_TYPE_BYTEBUF,
                                            s_ring_storage,
                                            &s_ring_struct);
    }
    if (s_pcm_ring == NULL) {
        ESP_LOGW(TAG, "PCM ring buffer alloc failed");
    }

    s_feeding = false;
    s_pending_rate = 0;
    audio_set_hpf_coeff(s_rate);      /* default-rate HPF coefficient */
    audio_set_loudness_coeff(s_rate); /* default-rate loudness shelf coeff */
    audio_build_vol_table();          /* precompute the 0.1 dB gain table */
    audio_update_vol_gain();
    s_vol_gain_sm = s_vol_gain;       /* start settled: no fade-in from zero */
    s_loud_boost_sm = s_loud_boost;   /* shelf boost settled too */
    s_lim_gain = 32768;               /* limiter flat until audio starts */
    BaseType_t feed_ret = xTaskCreate(audio_feed_task, "audio_feed",
                                      6 * 1024, NULL, 6, &s_feed_task);
    if (feed_ret != pdPASS) {
        ESP_LOGE(TAG, "[AUDIO] feed task create FAILED -> audio unusable");
        s_feed_task = NULL;
        s_ready = false;          /* mark unready so player never starts */
        return;
    }

    ESP_LOGI(TAG, "[AUDIO] I2S ready, RingBuffer created, Feed task started");

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

/* Request an I2S sample-rate change; the feed task applies it (serialized).
 * The Bluetooth pipeline is only told when Bluetooth is the active route, so a
 * speaker session never touches the BT resampler (and stays silent on the BT
 * side). */
void hw_audio_set_sample_rate(uint32_t sample_rate_hz)
{
    s_pending_rate = sample_rate_hz;
    if (audio_route_is_bt()) {
        bt_audio_set_sample_rate(sample_rate_hz);
    }
}

/* Discard everything currently queued in the PCM ring (e.g. leftovers from
 * the previous track) WITHOUT touching the feed task or parking the channel.
 * The next samples enqueued start the stream cleanly at the new track's rate.
 * Safe to call from the player task; the ring is accessed lock-free here
 * because the feed task only RECEIVES (never sends) into it. */
void hw_audio_flush(void)
{
    if (s_pcm_ring == NULL) {
        return;
    }
    uint8_t *item;
    size_t item_size;
    while ((item = (uint8_t *)xRingbufferReceive(s_pcm_ring, &item_size, 0)) != NULL) {
        vRingbufferReturnItem(s_pcm_ring, item);
    }
    ESP_LOGD(TAG, "pcm ring flushed");
}

/* Mark/unmark the MP3 player as the owner of the I2S bus. */
void hw_audio_set_player_active(bool active)
{
    /* 原则3：audio 未 ready（I2S/ring/feed task 未建好）时禁止启动播放。
     * 否则 feed task 不存在，ring 永不被消费，decode 卡死。 */
    if (active && !s_ready) {
        ESP_LOGE(TAG, "[ERROR] player active requested but audio not ready");
        return;
    }
    s_player_active = active;
    if (active) {
        s_feeding = true;
        /* NOTE: s_pending_rate is intentionally NOT cleared here. The first
         * decoded frame sets it (via hw_audio_set_sample_rate) and the feed
         * task applies it. Clearing it on activate would discard that rate
         * whenever set_player_active(true) is deferred until after the first
         * frame (as the player does), leaving the I2S clock stuck at the
         * previous track's rate — which makes the next track silent. */
        audio_dsp_reset();            /* fresh filter history per track */
        s_vol_gain_sm = s_vol_gain;   /* start at full gain: no fade-in */
        audio_set_hpf_coeff(s_rate);  /* default-rate coeff until 1st frame */
        audio_set_loudness_coeff(s_rate);
        ESP_LOGD(TAG, "player active: DSP reset, rate=%u", (unsigned)s_rate);
        ESP_LOGI(TAG, "[PLAYER] audio pipeline ready");
        /* 唤醒前先确认 feed task 存在（s_feed_task 在 init 失败时为 NULL）。 */
        if (s_feed_task != NULL) {
            xTaskNotifyGive(s_feed_task);
        }
        else {
            ESP_LOGE(TAG, "[ERROR] player active but feed task NULL (init failed)");
        }
    }
    else {
        s_feeding = false;            /* feed task drains the ring then idles */
        s_pending_rate = 0;           /* drop any unapplied rate from the track */
        if (s_feed_task != NULL) {
            xTaskNotifyGive(s_feed_task);   /* wake it to drain/park now */
        }
        ESP_LOGD(TAG, "player inactive: draining ring");
    }
}

bool hw_audio_is_ready(void)
{
    return s_ready && (s_feed_task != NULL) && (s_pcm_ring != NULL);
}

/* Enqueue decoded 16-bit stereo PCM (L,R interleaved). `frames` = number of
 * L/R pairs.
 *
 * Output routing (like a phone): while a Bluetooth sink is linked, audio goes
 * to the headphones ONLY — full band (no speaker high-pass, headphones can
 * reproduce bass) with master volume applied. The I2S path is skipped
 * entirely so the A2DP data callback is the ONE and only pacer of the decode
 * task (via the blocking send into the BT ring). Feeding both I2S and BT at
 * once would tie the decoder to two independent clocks; any drift between
 * them periodically drains one of the rings and causes dropouts.
 *
 * Without Bluetooth, the speaker path applies the protection high-pass,
 * the loudness bass shelf, the master volume and the soft limiter.
 * Back-pressure blocks until there is room (PCM is never dropped); if
 * playback stops meanwhile we abandon the rest. */
void hw_audio_write_pcm(int16_t *stereo_frames, size_t frames)
{
    if (!s_ready || s_pcm_ring == NULL || !s_player_active || frames == 0) {
        return;
    }

    /* Route is a single explicit decision held in s_route (set only via
     * hw_audio_set_route). We never probe the Bluetooth link here; audio goes
     * to exactly one destination for this whole call. */
    const bool bt_out = (s_route == AUDIO_ROUTE_BT);

    const int32_t g_target = s_vol_gain;    /* Q15 target logarithmic gain */
    int32_t g = s_vol_gain_sm;              /* smoothed gain to apply */
    size_t n = frames * 2;

    if (bt_out) {
        /* Bluetooth route: volume only, full band. The blocking send inside
         * bt_audio_write_pcm() paces the decoder; the speaker feed is parked by
         * the feed task (see audio_route_is_bt) so the I2S path is truly silent,
         * not just starved. */
        for (size_t i = 0; i < n; i++) {
            g += ((g_target - g) * VOL_SMOOTH_A_Q15) >> 15;
            stereo_frames[i] = (int16_t)(((int32_t)stereo_frames[i] * g) >> 15);
        }
        s_vol_gain_sm = g;
        bt_audio_write_pcm(stereo_frames, frames);
        return;
    }

    /* Speaker route: high-pass -> loudness bass shelf -> master volume
     * -> soft limiter, all per sample (L = even index, R = odd index). */
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
    int waits = 0;
    /* 有限重试：ring 满时最多等 ~2s。若 feed task 未运行（pipeline 卡死），
     * 这里必须退出并返回，而不是永久阻塞 decode task（否则日志卡死、
     * 设备再也无法播放）。原则2：禁止无超时永久阻塞。 */
    const int PCM_SEND_MAX_WAITS = 40;   /* 40 * 50ms = ~2s */
    while (xRingbufferSend(s_pcm_ring, stereo_frames, bytes,
                           pdMS_TO_TICKS(50)) != pdPASS) {
        if (!s_player_active) {
            return;
        }
        if (++waits >= PCM_SEND_MAX_WAITS) {
            ESP_LOGE(TAG, "[ERROR] PCM buffer full timeout (AUDIO PIPELINE STALLED)");
            return;
        }
    }
    if (waits > 0) {
        ESP_LOGD(TAG, "pcm back-pressure: waited %d x50ms (frames=%u)", waits,
                 (unsigned)frames);
    }
    if (s_feed_task != NULL) {
        /* Event-driven feed: wake it on data instead of leaving it to sit
         * out its 100 ms receive timeout (startup / post-underrun latency). */
        xTaskNotifyGive(s_feed_task);
    }
}

