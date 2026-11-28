/*
 * Hardware layer: I2S audio output driving a MAX98357 Class-D DAC.
 * See audio.h.
 *
 * Wiring (from board_config.h):
 *   BCLK -> GPIO32, LRC(WS) -> GPIO15, DIN -> GPIO21, no MCLK.
 *
 * The I2S bus is configured as 16-bit STEREO (fixed 44100 Hz) and driven by a
 * self-contained i2s_std writer task; raw PCM is streamed for MP3 playback.
 *
 * Architecture: DIRECT decode-to-I2S path with a SMALL ring buffer (freertos
 * ringbuf) between the DSP and the DMA writer task. The MP3 player task (the
 * only writer) applies the DSP chain and pushes PCM into the ring via
 * hw_audio_write_pcm(); the writer task pops the ring and feeds the i2s_std
 * DMA. The DMA is the jitter buffer and paces the decoder by back-pressure, so
 * decode and the hardware clock can never run away from each other.
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
#include "bluetooth_audio.h"
#include "speex/speex_resampler.h"   /* OUTSIDE_SPEEX + FIXED_POINT: self-contained
                                     * fixed-point arbitrary-rate resampler */

#include <math.h>
#include <string.h>

#include "driver/i2s_std.h"   /* i2s_chan_handle_t / i2s_channel_init_std_mode / i2s_gpio_config_t */
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"          /* EXT_RAM_BSS_ATTR for the resampler scratch */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"    /* TaskHandle_t / xTaskCreate / ulTaskNotifyTake / xTaskNotifyGive */
#include "freertos/ringbuf.h" /* xRingbuffer*: local PCM ring between the DSP and the DMA writer task */
#include "freertos/semphr.h"
#if defined(CONFIG_ESP_TASK_WDT_EN)
#include "esp_task_wdt.h"   /* esp_task_wdt_reset: keep the task WDT fed while we block */
#endif

static const char *TAG = "hw_audio";

#define AUDIO_DEFAULT_RATE  44100
#define AUDIO_2PI           6.2831853f

/* I2S DMA ring: 12 descriptors x 1024 frames (x 4 bytes/frame, 16-bit
 * stereo) = 48 KB of buffering (~280 ms at 44.1 kHz). */
#define I2S_DMA_DESC_NUM    12

/* Short silence prime AFTER re-enabling the channel. The full 48 KB descriptor
 * overwrite happens BEFORE disable() inside i2s_seam_rearm(); this 2 KB prime
 * (~12 ms) only keeps the freshly re-armed out-link fed while the first new
 * PCM chunk is prepared. It is deliberately NOT a substitute for the full
 * overwrite: ESP-IDF documents that disable() does not flush the TX descriptor
 * queue, so a re-arm without the preceding full-ring silence replays the
 * previous track's stale descriptors ("上一首残音"). */
#define SEAM_SILENCE_PRIME_BYTES (2U * 1024U)

/* Track-switch fade-out: 20 ms cosine ramp over the previous track's PCM still
 * queued in the ring when a hard switch is requested. The seam still needs a
 * full 48 KB silence overwrite (see i2s_seam_rearm), so without a ramp the cut
 * to silence is a hard waveform jump = click. Industry practice is 10-50 ms
 * with a cosine (gentler than linear near the top) envelope; 20 ms / 64-step
 * Q15 table keeps it integer-only and cheap in the DMA writer task. */
#define FADE_OUT_MS          20
#define FADE_OUT_STEPS       64
#define FADE_OUT_FRAMES      ((AUDIO_DEFAULT_RATE * FADE_OUT_MS) / 1000)
static const int16_t s_fade_cos_q15[FADE_OUT_STEPS + 1] = {
    32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285,
    32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571,
    30273, 29956, 29621, 29268, 28898, 28510, 28105, 27683,
    27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731,
    23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868,
    18204, 17530, 16846, 16151, 15446, 14732, 14010, 13279,
    12539, 11793, 11039, 10278,  9512,  8739,  7962,  7179,
     6393,  5602,  4808,  4011,  3212,  2410,  1608,   804,
        0
};

/* Set by hw_audio_pipeline_switch(): the writer fades the queued old-track PCM
 * to zero before dropping the rest and re-arming. */
static bool s_flush_fadeout;

/* Time for the DMA to flush its in-flight descriptors (~48 KB at 44.1 kHz
 * stereo ≈ 280 ms) after the PCM ring has drained. The writer waits this out
 * before parking and hw_audio_drain_blocking() adds it on top, so a parked or
 * track-ended boundary does not chop the last ~280 ms of audio. The refactor
 * to a ring buffer + async writer had no such wait, so "the song ended" was
 * both inaudible (tail dropped on park) and undetectable (decoder EOF led real
 * playback by ~300 ms with no completion signal). */
#define I2S_DMA_DRAIN_MS    350

/* PCM ring between the DSP (decode task) and the DMA writer task: 32 KB
 * (~186 ms at 44.1 kHz stereo). Sizing rationale: the 48 kHz -> 44.1 kHz
 * resample path runs the producer at ~25 ms/frame against a 24 ms frame budget,
 * so SD-read jitter (measured 4 KB reads ~25-30 ms, >50 ms under SPI2
 * contention) used to drain the 16 KB / 93 ms ring and make the writer feed
 * silence mid-song ("[AUDIO] I2S underrun: ring empty while playing"). 32 KB
 * doubles the jitter absorption. This ring is the only buffer in front of the
 * ~279 ms DMA ring; cost is pipeline latency one-for-one on track-end drain /
 * pause / stop tail (+93 ms), which the seam flush already waits out. */
#define PCM_RING_BYTES       (32 * 1024)

/* The BYTEBUF ring coalesces consecutive 1 KB producer chunks into one large
 * item. If the writer receives 16~26 KB at once, i2s_channel_write() blocks for
 * the real-time duration of that audio (~90~137 ms) and cannot return the item
 * to the ring, so the producer's 100 ms xRingbufferSend() times out even though
 * s_ring_bytes says the ring is mostly free. Cap each writer pull to one
 * 44.1 kHz stereo DMA descriptor (~23 ms) so the item is returned promptly. */
#define PCM_WRITER_CHUNK_MAX (4 * 1024)

/* A dedicated DMA writer task owns the actual I2S write, so there is no manual
 * write timeout / IO mutex here: hw_audio_write_pcm() blocks on the ring buffer
 * (xRingbufferSend) and the writer task feeds the i2s_std DMA independently. */

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

/* Local I2S output: we replace the ADF i2s_stream element with a
 * self-contained writer. A freertos ringbuf buffers DSP-produced PCM; a
 * dedicated task pulls from it and feeds the ESP-IDF i2s_std DMA channel, so
 * there is no ADF dependency and no manual IO mutex — xRingbufferSend() blocks
 * on the ring and the writer task paces the DMA independently. */
static i2s_chan_handle_t s_i2s_chan = NULL;  /* i2s_std TX channel (the DMA)   */
static RingbufHandle_t   s_pcm_rb    = NULL;  /* DSP -> DMA PCM ring buffer     */
static TaskHandle_t      s_i2s_task  = NULL;  /* DMA writer task handle         */
static bool              s_i2s_disabled;      /* channel currently disabled     */
static bool              s_parked;            /* hw_audio_park(): stop BCLK     */
static bool              s_flush_req;         /* pipeline_flush pending         */
static bool              s_flush_fill;        /* hard switch: also overwrite the
                                               * DMA descriptor ring with silence
                                               * to kill previous-track PCM left
                                               * in descriptors ahead of the DMA. */
static SemaphoreHandle_t s_flush_done = NULL; /* posted by the writer when a
                                               * synchronous pipeline flush has
                                               * actually dropped the ring + reset
                                               * the DMA queue, so the caller can
                                               * start the next track clean. */
/* One-shot seam diagnostics (see hw_audio_set_player_active + i2s_writer_task).
 * s_diag_arm: when set, the writer dumps its state the moment a seam flush
 * completes — proves whether s_player_active / s_parked / s_i2s_disabled are
 * correct at the instant track2 should start. s_diag_first: when set, the
 * writer logs the FIRST real PCM chunk it hands to the DMA after the seam —
 * if that prints with a real post_peak but the speaker stays silent, the PCM
 * reached the out-link and the fault is silicon/DMA-side, not the writer. */
static bool              s_diag_arm   = false;
static bool              s_diag_first = false;
static volatile bool s_ready;
static uint32_t s_rate = AUDIO_DEFAULT_RATE;
/* File-scope silence chunk used to prime / re-arm the DMA descriptor ring
 * across a track seam and at park. Must be reachable from i2s_seam_rearm(). */
static int16_t s_seam_silence[256 * 2];

/* The I2S bus runs at ONE FIXED rate (s_rate) for the whole session: decoded
 * PCM is resampled to it in hw_audio_write_pcm, so a rate change NEVER
 * reconfigures the I2S clock. The i2s_std channel is enabled once at init and
 * left RUNNING; the writer task pause/resume (not a channel disable/enable)
 * gates it. This removes the out-link stop/start cycle that wedged the ESP32
 * DMA ("plays at first,
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
 * "I2S write gap" underrun warnings appear. q1 (filt_len 16) is ~5x cheaper,
 * still a true windowed-sinc (vastly better than the old linear interpolator),
 * and leaves huge real-time slack for decode + DSP on core 0. */
#define RESAMP_QUALITY     1      /* 0..10; ESP32 sweet spot (fast + sinc quality) */
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

/* Route-change listener (see hw_audio_set_route_change_cb). Invoked from
 * audio_apply_route() after the new route is fully in effect. */
static audio_route_change_cb_t s_route_change_cb;

/* Bluetooth-route volume-change listener (see
 * hw_audio_set_bt_volume_change_cb). Invoked whenever the BT slot's value
 * changes while the BT route is the destination for it, so the app can mirror
 * the volume to a connected A2DP sink. */
static hw_audio_bt_volume_cb_t s_bt_vol_cb;

/* Consecutive failed PCM ring writes (bounds the WARN/ERROR rate). */
static uint32_t s_wr_errs;
/* Consecutive timed-out i2s_channel_write()s (bounds the ERROR rate). Surfaces
 * a wedged DMA as a logged stall instead of a silent portMAX_DELAY freeze. */
static uint32_t s_wr_stalls;

/* Diagnostic-only I2S write timing. With the writer capped at one DMA
 * descriptor (~23 ms at 44.1 kHz), only log writes clearly above that. Nothing
 * here changes the write timeout or retry behavior. */
#define I2S_SLOW_WRITE_US      30000
static uint32_t s_i2s_write_fails;
static uint32_t s_i2s_slow_writes;
static volatile int64_t s_last_i2s_write_dur_us;
static int64_t s_i2s_fail_log_us;
static int64_t s_i2s_slow_log_us;

/* Bytes currently sitting in the PCM ring: pushed by hw_audio_write_pcm(),
 * pulled by the writer task. This is how much decoded audio is still queued
 * but NOT yet handed to the DMA. Together with the DMA's own in-flight
 * descriptors (bounded ~280 ms, see I2S_DMA_DRAIN_MS) it tells the player
 * exactly when a track's last sample has actually been emitted — i.e. when
 * the song has really ended, instead of merely when the decoder hit EOF.
 * Without it the post-refactor player cannot determine track end.
 *
 * IMPORTANT: `volatile`. It is written by hw_audio_write_pcm() (decode task)
 * and by the writer task, and read across tasks by hw_audio_drain_blocking()/
 * hw_audio_pending(). Without volatile the compiler caches it in a register
 * inside the drain wait-loop and the loop spins forever (the writer empties
 * the ring, the cached value stays > 0) — which starves esp_task_wdt_reset()
 * and reboots via the 5 s task-WDT. Updates are also serialised by the
 * spinlock so the read-modify-write never loses an increment from the other
 * task. */
static volatile size_t s_ring_bytes;

/* Serialises the s_ring_bytes accounting across the decode task (increment)
 * and the writer task (decrement) so the non-atomic read-modify-write can
 * never lose an update. */
static portMUX_TYPE s_ring_lock = portMUX_INITIALIZER_UNLOCKED;

/* Diagnostic-only consumer state. These used to be static locals inside the
 * writer task; they live at file scope only so the producer's "PCM ring full"
 * path can log what the writer/DMA side was doing. No audio behavior changes. */
static uint32_t s_w_chunks;       /* real-PCM chunks handed to DMA */
static uint32_t s_sil_chunks;     /* silence chunks emitted */
static uint32_t s_drop_inactive;  /* real PCM dropped because !active */
static int32_t  s_post_peak;      /* peak of POST-DSP PCM sent to DMA */
static volatile int64_t s_last_ring_pull_us;
static volatile int64_t s_last_real_dma_us;
static volatile int64_t s_last_flush_done_us;

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

static uint8_t s_vol_speaker = 30;       /* per-route volume, percent */
static uint8_t s_vol_bt      = 30;
static uint8_t s_volume = 30;            /* active route volume (UI view) */
static int32_t s_vol_gain;               /* target Q15 linear gain */
static int32_t s_vol_gain_sm;            /* smoothed gain actually applied */

/* Perceptual volume taper (built once at init into s_vol_tab / s_vol_tab_bt).
 *
 * A plain quadratic gain (v/100)² is concave in dB: near full scale a 5% step
 * is <1 dB, below the ear's just-noticeable-difference, so the top of the
 * range feels "stuck". We instead map percent linearly to attenuation in dB
 * (a true audio/log taper) so EVERY percent is the same number of dB and the
 * difference between adjacent settings is always clearly audible:
 *
 *     gain_dB(v) = (v/100 - 1) * MAX_ATTEN_DB(route)   (0 dB at v=100)
 *     gain_lin   = 10^(gain_dB / 20)
 *     Q15        = gain_lin * 32767
 *
 *   speaker route (0..-40 dB, matches the MAX98357 amp's usable range):
 *   vol   gain(Q15)  level(dB)   5% step (dB)
 *   100   32767        0.0        ~2.0 each (constant & obvious)
 *    70    8231      -12.0
 *    50    3277      -20.0
 *    30    1304      -28.0
 *    10     519      -36.0
 *     0       0        -∞          mute
 *
 *   BT route (0..-12 dB): the A2DP sink applies its OWN volume — we mirror
 *   ours to it via AVRCP absolute volume, so the sink attenuates AGAIN on
 *   its side. A deep local taper would MULTIPLY with the sink's and the low
 *   end of the knob went silent (25 % used to be ≈ -30 dB local × the sink's
 *   own ≈ -25 dB ≈ -55 dB total). The BT route therefore only trims locally
 *   (±12 dB of authority); the sink does the coarse attenuation. 25 % now
 *   lands at ≈ -9 dB local, clearly audible through any sink setting.
 *
 * Both tables are built at 0.1 dB resolution (not per-percent) so the AVRCP
 * remote scale (0..127) also resolves to uniform steps. Mapping it through
 * the coarse percent grid instead made consecutive remote presses alternate
 * "no change / one step" and feel uneven. */
#define VOL_SPK_MAX_ATTEN_DB 40  /* speaker: total attenuation at v=0; every 5% press
                                  * ≈ 2 dB — clearly audible */
#define VOL_BT_MAX_ATTEN_DB  12  /* BT: local trim only; the sink mirrors the rest */
#define VOL_TAB_STEP_DB  0.1f    /* table resolution: 0.1 dB per entry */
#define VOL_TAB_ENTRIES(MAX_DB)  ((int)((MAX_DB) * 10) + 1)  /* depth / 0.1 dB + 0 dB */
static int32_t s_vol_tab[VOL_TAB_ENTRIES(VOL_SPK_MAX_ATTEN_DB)];    /* 401: 0..-40 dB */
static int32_t s_vol_tab_bt[VOL_TAB_ENTRIES(VOL_BT_MAX_ATTEN_DB)];  /* 121: 0..-12 dB */

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

/* Index into a taper table of depth `max_db` for the attenuation `dB`
 * (0 at full scale, -max_db at silence). Rounded to the nearest 0.1 dB
 * entry and clamped to the table ends. */
static int vol_tab_index(float dB, float max_db)
{
    if (dB <= -max_db) {
        return 0;
    }
    if (dB >= 0.0f) {
        return (int)(max_db / VOL_TAB_STEP_DB);
    }
    return (int)((dB + max_db) / VOL_TAB_STEP_DB + 0.5f);
}

static void audio_build_vol_table(void)
{
    /* Speaker table: 0..-40 dB. */
    for (int i = 0; i < VOL_TAB_ENTRIES(VOL_SPK_MAX_ATTEN_DB); i++) {
        float dB = -(float)VOL_SPK_MAX_ATTEN_DB + i * VOL_TAB_STEP_DB;
        float g = powf(10.0f, dB / 20.0f);
        int32_t val = (int32_t)(g * 32767.0f + 0.5f);
        if (val > 32767) {
            val = 32767;
        }
        s_vol_tab[i] = val;
    }
    /* BT table: 0..-12 dB (see the taper comment above). */
    for (int i = 0; i < VOL_TAB_ENTRIES(VOL_BT_MAX_ATTEN_DB); i++) {
        float dB = -(float)VOL_BT_MAX_ATTEN_DB + i * VOL_TAB_STEP_DB;
        float g = powf(10.0f, dB / 20.0f);
        int32_t val = (int32_t)(g * 32767.0f + 0.5f);
        if (val > 32767) {
            val = 32767;
        }
        s_vol_tab_bt[i] = val;
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

/* Active route's taper depth (dB) and gain table. The BT route's shallow
 * 12 dB depth is what keeps the low end of the knob audible: the sink
 * mirrors our percent and attenuates again on its side (see the taper
 * comment above). */
static float vol_active_max_db(void)
{
    return audio_route_is_bt() ? (float)VOL_BT_MAX_ATTEN_DB
                               : (float)VOL_SPK_MAX_ATTEN_DB;
}

static const int32_t *vol_active_tab(void)
{
    return audio_route_is_bt() ? s_vol_tab_bt : s_vol_tab;
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
        /* 路由到 BT: writer 在下一轮自动 pause(BCLK 停,MAX98357 休眠)。
           扬声器路径不再写它,切回时 resume 即可。 */
    }
    /* else: 切回扬声器, writer 在下一轮自动 resume。唤醒 writer 让变动即时生效。 */
    if (s_i2s_task != NULL) {
        xTaskNotifyGive(s_i2s_task);
    }
    s_volume = audio_route_is_bt() ? s_vol_bt : s_vol_speaker;
    audio_update_vol_gain();
    audio_dsp_reset();
    s_vol_gain_sm = s_vol_gain;   /* no fade-in on route switch */
    ESP_LOGI(TAG, "audio route -> %s (vol %u%%)",
             audio_route_is_bt() ? "bluetooth" : "speaker", (unsigned)s_volume);
    /* Notify AFTER the route is fully applied. This runs on the switching
     * task (UI task, or the Bluetooth stack's link callback), so the listener
     * must stay cheap and must not call hw_audio_set_route() back (re-entrancy
     * is NOT guarded). */
    if (s_route_change_cb) {
        s_route_change_cb(s_route);
    }
}

void hw_audio_set_route(audio_route_t route)
{
    audio_apply_route(route);
}

audio_route_t hw_audio_get_route(void)
{
    return s_route;
}

void hw_audio_set_route_change_cb(audio_route_change_cb_t cb)
{
    s_route_change_cb = cb;
}

void hw_audio_set_bt_volume_change_cb(hw_audio_bt_volume_cb_t cb)
{
    s_bt_vol_cb = cb;
}

/* Publish the Bluetooth slot's volume to the listener (if any). Only called
 * on paths that change the slot WHILE the BT route is active, i.e. when the
 * new value is the one the sink should mirror. Inbound AVRCP volume never
 * comes through here: the sink is already at that value. */
static void audio_publish_bt_volume(void)
{
    if (s_bt_vol_cb != NULL) {
        s_bt_vol_cb(s_vol_bt);
    }
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
    /* The shelf target must be refreshed even at v = 0: the early return below
     * used to leave the PREVIOUS volume's boost in place, so un-muting from
     * 0 % applied a stale shelf (up to +9 dB instead of the +8.6 dB the new
     * volume asks for) — a small bass jump on the first samples. */
    audio_update_loudness_boost();
    if (v == 0) {
        s_vol_gain = 0;                  /* hard mute */
        return;
    }
    const float max_db = vol_active_max_db();
    float dB = ((float)v / 100.0f - 1.0f) * max_db;
    s_vol_gain = vol_active_tab()[vol_tab_index(dB, max_db)];
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
 * ORDERING CONSTRAINT: the shelf must run AFTER the master volume gain. It
 * adds up to +9 dB, so on a full-scale signal it pushed the int16 sum past
 * full scale and the hard clamp there clipped it — and the clip amount grew
 * as the volume went down (the boost grows), i.e. the distortion came and
 * went with the knob. With the volume in front, g*(1+boost) <= 1 for every
 * knob position, so the shelf can never clip. The frequency response is
 * unchanged (the stages are linear: g*(y+b*lp(y)) == g*y + b*lp(g*y)).
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

/* --- Headroom + hard ceiling ---------------------------------------------
 * The gain stages (ReplayGain, master preamp) keep LIM_HEADROOM of integer
 * room instead of clipping to int16, so the DRC below sees the true peak and
 * can compress it rather than scale an already-square wave. The final hard
 * ceiling at the DAC / MAX98357 input is -2.5 dBFS: even the fastest DRC
 * attack transient cannot reach 0 dBFS. */
#define LOUD_LIMIT_CEIL       24576  /* hard ceiling at the DAC / amp input */
#define LIM_HEADROOM         524288  /* 16x FS: no clipping before the DRC */

/* --- Two-band dynamic range compressor (DRC + MBC) -----------------------
 * The old single-band limiter was a ∞:1 peak normaliser at -2.5 dBFS: a hot
 * track was ducked by the same hard ratio at EVERY frequency, so the loud
 * 1..4 kHz band (where this tiny driver is most efficient) was squashed just
 * as hard as the sub-400 Hz content that actually eats its ±0.22 mm excursion.
 * It also had a hard knee: gain changed abruptly at the threshold.
 *
 * This replaces it with a complementary 2-band split at 400 Hz:
 *
 *   low  = one-pole LP(x)        <- excursion risk: -20 dBFS, 4:1, soft knee
 *   high = x - low               <- audible band:    -6 dBFS, 3:1, soft knee
 *
 * The parameters were picked with a bit-exact simulation of the FULL chain
 * (HPF -> volume -> loudness shelf -> DRC) on a hot bass-heavy master: this
 * setting ducks the <400 Hz excursion band by ~2 dB while leaving 400 Hz..8 kHz
 * within 0.3 dB — i.e. the driver is protected without the mids being
 * squashed. Harder settings (-24 dBFS / 6:1) cost 5.5 dB of low end and were
 * rejected as inaudible on this 0.22 mm driver.
 *
 * Each band has its OWN peak envelope and its OWN smoothed gain (Q15), looked
 * up from a soft-knee gain curve indexed by the envelope's log2 (16 steps per
 * octave, 1..LIM_HEADROOM). Below a band's threshold the gain is exactly 1.0,
 * so quiet material is untouched; above it the gain falls smoothly (a 6 dB
 * knee) instead of snapping. The band gains are applied separately and then
 * summed — because low+high reconstructs x exactly, the sum stays continuous
 * at the crossover and no comb filtering is introduced.
 *
 * The hard ceiling (LOUD_LIMIT_CEIL) is kept AFTER the recombination as the
 * absolute last line of defence: even a fast attack transient cannot reach
 * 0 dBFS at the DAC / MAX98357 input. All maths is integer; the two curves
 * are compile-time tables generated from the soft-knee formula, so there is
 * no powf/logf in the audio ISR path. */
#define DRC_XOVER_FC_HZ        400    /* band split; below the 800 Hz HPF corner */
#define DRC_ENV_REL_Q15      32753    /* envelope decay factor (~100 ms) */
#define DRC_ATT_Q15           4096    /* gain attack alpha 0.125 (~0.3 ms) */
#define DRC_REL_Q15             24    /* gain release alpha ~0.00073 (~30 ms) */
#define DRC_TAB_STEPS         16      /* log2 steps per octave */
#define DRC_TAB_OCTAVES       20      /* env 1..2^19 (524288) */
#define DRC_TAB_SIZE        (DRC_TAB_STEPS * DRC_TAB_OCTAVES)  /* 320 */

/* DRC gain curves, Q15. Index = (msb<<4)|frac of the peak envelope,
 * 16 steps/octave over env 1..524288 (LIM_HEADROOM). Each table is the full
 * 320 entries: indices below the band threshold are exactly 0 dB (32768).
 * Storing the 0 dB prefix explicitly removes the per-sample `idx < off` branch
 * and the compact-table offset arithmetic from the hot DSP loop. Bit-exact
 * against the old compact+guard form (verified by simulation). */
#define DRC_TAB_OFF_LOW  178  /* T=-20dBFS R=4:1 K=6dB (0 dB prefix length) */
static const int16_t s_drc_gain_low[320] = {
    /* T=-20dBFS R=4:1 K=6dB.
       [0..177] 0 dB; [178..] curve; tail mirrors the old
       zero-filled array tail for bit-exactness. */
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32760, 32673, 32499, 32253, 31946, 31589, 31191, 30758, 30298, 29817,
    29318, 28807, 28286, 27759, 26963, 25902, 24859, 23896, 23017, 22209,
    21464, 20776, 20136, 19541, 18986, 18465, 17977, 17518, 17086, 16677,
    16105, 15410, 14781, 14209, 13686, 13206, 12763, 12353, 11973, 11619,
    11289, 10980, 10689, 10416, 10159, 9916, 9576, 9163, 8789, 8449,
    8231, 8231, 8231, 8231, 8231, 8231, 8231, 8231, 8231, 8231,
    8231, 8231, 8231, 8231, 8231, 8231, 8231, 8231, 8231, 8231,
    8231, 8231, 8231, 8231, 8231, 8231, 8231, 8231, 8231, 8231,
    8231, 8231, 8231, 8231, 8231, 8231, 8231, 8231, 8231, 8231,
    8231, 8231, 8231, 8231, 8231, 8231, 8231, 8231, 8231, 8231,
    8231, 8231, 8231, 8231, 8231, 8231, 8231, 8231, 8231, 8231,
    8231, 8231, 8231, 8231, 8231, 8231, 8231, 8231, 8231, 8231,
    8231, 8231, 8231, 8231, 8231, 8231, 8231, 8231, 8231, 8231,
    8231, 8231, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0,
};

static const int16_t s_drc_gain_high[320] = {
    /* T=-6dBFS R=3:1 K=6dB.
       [0..214] 0 dB; [215..] curve; tail mirrors the old
       zero-filled array tail for bit-exactness. */
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
    32768, 32768, 32768, 32768, 32768,
    32749, 32677, 32556, 32393, 32193, 31962, 31703, 31420, 31117, 30632,
    29938, 29208, 28454, 27688, 26916, 26147, 25400, 24704, 24054, 23445,
    22873, 22335, 21827, 21348, 20893, 20255, 19476, 18768, 18121, 17526,
    16979, 16472, 16001, 15563, 15153, 14770, 14409, 14070, 13750, 13448,
    13162, 12760, 12269, 11823, 11415, 11041, 10696, 10377, 10080, 9804,
    9546, 9304, 9077, 8864, 8662, 8472, 8292, 8038, 7729, 7448,
    7191, 6955, 6738, 6537, 6350, 6176, 6014, 5861, 5718, 5584,
    5457, 5337, 5223, 5064, 4869, 4692, 4530, 4382, 4245, 4118,
    4000, 3891, 3788, 3692, 3602, 3518, 3438, 3362, 3291, 3190,
    3067, 2956, 2854, 2760, 2674, 2594, 2520, 2451, 2386, 2326,
    2269, 2216, 2166, 2118, 2073,
};

static int32_t s_drc_lp[2];       /* crossover low-pass state, per channel */
static int32_t s_drc_lp_coeff;    /* Q15 one-pole alpha for DRC_XOVER_FC_HZ */
static int32_t s_drc_env[2][2];   /* [band][ch] peak envelope */
static int32_t s_drc_gain[2][2];  /* [band][ch] smoothed Q15 gain */

/* Envelope -> gain-table index: 16 log2 steps per octave, msb 0..19. */
static inline int drc_gain_index(int32_t env)
{
    if (env < 1) {
        return 0;
    }
    uint32_t v = (uint32_t)env;
    int msb = 31 - __builtin_clz(v);
    int frac = (msb >= 4) ? (int)((v >> (msb - 4)) & 0xF)
                          : (int)((v << (4 - msb)) & 0xF);
    return (msb << 4) | frac;
}

/* Process one DRC band and return its gain-scaled sample. `b` is always a
 * compile-time constant (0/1) at the call sites, so the compiler folds the
 * table/env/gain selects and inlines this into two straight-line paths — no
 * loop, no `idx < off` guard, no compact-table offset arithmetic. Bit-exact
 * against the old in-loop form (full-table simulation verified). */
static inline int32_t drc_apply_band(int b, int32_t in, int ch)
{
    int32_t a = (in < 0) ? -in : in;
    int32_t *env = &s_drc_env[b][ch];
    if (a > *env) {
        *env = a;                       /* instant peak attack */
    }
    else {
        *env = (int32_t)(((int64_t)*env * DRC_ENV_REL_Q15) >> 15);
    }
    int32_t idx = drc_gain_index(*env);
    int32_t tg = (b == 0) ? s_drc_gain_low[idx] : s_drc_gain_high[idx];
    int32_t *g = &s_drc_gain[b][ch];
    if (tg < *g) {
        *g += ((tg - *g) * DRC_ATT_Q15) >> 15;
    }
    else {
        *g += ((tg - *g) * DRC_REL_Q15) >> 15;
    }
    return (int32_t)(((int64_t)in * *g) >> 15);
}

/* Recompute the crossover low-pass coefficient for a new (fixed I2S) rate. */
static void audio_set_drc_coeff(uint32_t rate)
{
    if (rate == 0) {
        return;
    }
    float alpha = 1.0f - expf(-AUDIO_2PI * (float)DRC_XOVER_FC_HZ
                              / (float)rate);
    int32_t q = (int32_t)(alpha * 32768.0f);
    if (q < 1) {
        q = 1;
    }
    else if (q > 32767) {
        q = 32767;
    }
    s_drc_lp_coeff = q;
}

/* --- Per-track loudness gain (ReplayGain) -------------------------------
 * Different sources are mastered at wildly different levels; a per-track
 * gain computed from the file's ReplayGain 2.0 tags (written by PC tools
 * like loudgain) brings them to a common loudness. The player sets it at
 * each track start via hw_audio_set_track_gain_db(); it is smoothed like
 * the volume, so a gain step at a track boundary never clicks. Applied
 * BEFORE the user master gain on BOTH routes, so it scales the whole signal
 * the way a mastering gain would — the soft limiter below still caps the
 * peaks, which is exactly what ReplayGain assumes (limiting at 0 dBFS). */
#define RG_GAIN_MAX_DB 12.0f   /* clamp; loudgain values are normally ±6 dB */
static int32_t s_track_gain;    /* target Q15 per-track gain (32768 = 0 dB) */
static int32_t s_track_gain_sm; /* smoothed gain actually applied */

/* --- Master gain (user preamp) ------------------------------------------
 * A global -12..+12 dB gain applied to BOTH routes right after the per-track
 * ReplayGain (and, on the speaker route, before the soft limiter, so hot
 * peaks stay bounded). It shifts the whole signal
 * like a preamp knob: quiet sources can be pushed past what the volume knob
 * reaches at 100%, hot ones tamed. Persisted by the UI; smoothed like the
 * volume, so a change never clicks. */
#define MASTER_GAIN_MAX_DB 12.0f
static float    s_master_gain_db;   /* last set value, dB */
static int32_t  s_master_gain;      /* target Q15 gain (32768 = 0 dB) */
static int32_t  s_master_gain_sm;   /* smoothed gain actually applied */

/* Clear all per-track DSP history (call when a track starts or the route
 * switches). Both DRC bands reset to flat gain so the first samples of a new
 * track are never ducked by the previous track's peak envelopes. */
static void audio_dsp_reset(void)
{
    s_hpf_x1[0] = s_hpf_x1[1] = 0;
    s_hpf_y1[0] = s_hpf_y1[1] = 0;
    s_lp_state[0] = s_lp_state[1] = 0;
    s_drc_lp[0] = s_drc_lp[1] = 0;
    for (int b = 0; b < 2; b++) {
        s_drc_env[b][0] = s_drc_env[b][1] = 0;
        s_drc_gain[b][0] = s_drc_gain[b][1] = 32768;
    }
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
 * because it never touches the I2S DMA (bluetooth_audio_write_pcm() goes straight to
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
        audio_set_drc_coeff(s_rate);
        /* 恢复 writer(若被 park/路由切换暂停过)。通道在 init 时已 enabled,
           首次播放为无害 no-op。 */
        s_parked = false;
        if (s_i2s_task != NULL) {
            /* Arm the one-shot seam diagnostics: the writer will dump its
             * state right after the next flush completes, and log the first
             * real PCM chunk it hands to the DMA. This is how we tell, for a
             * silent second track, whether the writer is dropping PCM
             * (s_player_active stuck false), parked, or actually writing it. */
            s_diag_arm = true;
            s_diag_first = true;
            xTaskNotifyGive(s_i2s_task);
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
         * The writer's real-underrun counter needs no reset here: an inactive
         * player takes the mute branch, not the ring-empty branch. */
        ESP_LOGD(TAG, "player inactive: I2S left running (auto_clear silence)");
    }
}

/* Really stop playback: pause the DMA writer task. This stops BCLK and
 * powers the MAX98357 down — but does NOT disable/rebuild the I2S channel, so
 * there is no out-link stop/start cycle that could wedge the ESP32 DMA. The
 * writer is resumed by the next hw_audio_set_player_active(true) (or by a
 * route switch back to the speaker).
 *
 * Call this ONLY when audio is genuinely finished — when the decode loop exits
 * (stop / watchdog / give-up). It is deliberately NOT called on pause or
 * between tracks. */
void hw_audio_park(void)
{
    /* 暂停 writer:BCLK 停,MAX98357 休眠。Ring 中残留由 writer 在 pause 前自然
       排空;下次播放 resume 即干净。不再 disable/重建 I2S。 */
    s_parked = true;
    if (s_i2s_task != NULL) {
        xTaskNotifyGive(s_i2s_task);
    }
    ESP_LOGD(TAG, "I2S parked (writer paused)");
}

bool hw_audio_is_ready(void)
{
    return s_ready && (s_i2s_chan != NULL);
}

/* Bounded I2S DMA write. Returns true on success, false on timeout.
 *
 * The DMA writer task is NOT task-WDT monitored, so an infinite-block
 * i2s_channel_write() (a wedged out-link) froze playback with NO log and NO
 * reboot — the "press play, totally stuck" symptom. A bounded write instead
 * surfaces a wedged DMA as a logged stall (and retries next loop, which self-
 * heals a transient credit hiccup). A descriptor frees every ~23 ms at 44.1 kHz,
 * so a 50 ms timeout covers normal back-pressure and only trips on a real wedge.
 * On failure the caller keeps ownership of `data` (it must return/drop it). */
static bool i2s_write_bounded(const void *data, size_t len)
{
    size_t w = 0;
    int64_t t0 = esp_timer_get_time();
    esp_err_t rc = i2s_channel_write(s_i2s_chan, data, len, &w,
                                     pdMS_TO_TICKS(50));
    int64_t dur_us = esp_timer_get_time() - t0;
    int64_t now_us = t0 + dur_us;

    taskENTER_CRITICAL(&s_ring_lock);
    s_last_i2s_write_dur_us = dur_us;
    taskEXIT_CRITICAL(&s_ring_lock);

    if (rc == ESP_OK) {
        s_wr_stalls = 0;
        if (dur_us > I2S_SLOW_WRITE_US || w < len) {
            s_i2s_slow_writes++;
            if (now_us - s_i2s_slow_log_us > 1000000) {
                ESP_LOGW(TAG,
                         "[WRDBG] slow i2s write: len=%u written=%u dur=%lld us "
                         "fails=%u slow=%u",
                         (unsigned)len, (unsigned)w, (long long)dur_us,
                         (unsigned)s_i2s_write_fails,
                         (unsigned)s_i2s_slow_writes);
                s_i2s_slow_log_us = now_us;
            }
        }
        return true;
    }

    s_i2s_write_fails++;
    if (++s_wr_stalls >= 8) {
        ESP_LOGE(TAG, "[ERROR] i2s_channel_write stalled (rc=%d): DMA wedged?",
                 (int)rc);
        s_wr_stalls = 0;
    }
    if (s_i2s_write_fails <= 3 || now_us - s_i2s_fail_log_us > 1000000) {
        ESP_LOGE(TAG,
                 "[WRDBG] i2s write fail: len=%u written=%u rc=%d dur=%lld us "
                 "fails=%u slow=%u stalls=%u",
                 (unsigned)len, (unsigned)w, (int)rc, (long long)dur_us,
                 (unsigned)s_i2s_write_fails, (unsigned)s_i2s_slow_writes,
                 (unsigned)s_wr_stalls);
        s_i2s_fail_log_us = now_us;
    }
    return false;
}

/* Seam re-arm: overwrite the whole TX descriptor ring with silence WHILE the
 * channel is still ENABLED, then disable+enable to restart the out-link, then
 * a short silence prime. Order matters: ESP-IDF documents i2s_channel_disable()
 * as only stopping BCLK/WS — it does NOT flush the TX descriptor queue. With
 * the old order (disable+enable first, then a 2 KB prime) the re-enabled DMA
 * replayed stale descriptors still holding the previous track's PCM — exactly
 * the reported "上一首残音". Writing a full ring of silence up front makes
 * every descriptor silent before the re-arm can replay them.
 *
 * The disable+enable itself stays: on this i2s_std build a drain-only seam
 * left the next track silent ("second song dead"), and the ADF driver's
 * "enable() doesn't refill the credit queue" caveat does not apply here.
 * The full overwrite costs ~280 ms of writer time, so both synchronous flush
 * callers wait long enough (400 ms) before the decode loop starts writing the
 * new track — that is what keeps the PCM ring from overflowing at the seam. */
static void i2s_seam_rearm(void)
{
    if (s_i2s_chan == NULL || s_i2s_disabled) {
        return;
    }
    /* Silence EVERY descriptor before disable(): a re-arm only replays what is
     * already in the ring, so any descriptor skipped here comes back as the
     * previous track's tail. */
    size_t overwritten = 0;
    const size_t ring_bytes = I2S_DMA_DESC_NUM * 1024U * 4U;
    while (overwritten < ring_bytes) {
        if (!i2s_write_bounded(s_seam_silence, sizeof(s_seam_silence))) {
            break;   /* stalled link: cannot guarantee a clean seam */
        }
        overwritten += sizeof(s_seam_silence);
    }
    esp_err_t rc = i2s_channel_disable(s_i2s_chan);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "[AUDIO] seam re-arm: disable rc=%s", esp_err_to_name(rc));
    }
    rc = i2s_channel_enable(s_i2s_chan);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "[AUDIO] seam re-arm: enable rc=%s", esp_err_to_name(rc));
        return;
    }
    size_t filled = 0;
    while (filled < SEAM_SILENCE_PRIME_BYTES) {
        if (!i2s_write_bounded(s_seam_silence, sizeof(s_seam_silence))) {
            break;
        }
        filled += sizeof(s_seam_silence);
    }
    ESP_LOGW(TAG, "[AUDIO] seam re-arm: overwrite %u B + reset + %u B prime",
             (unsigned)overwritten, (unsigned)filled);
}

/* Create the I2S output with ESP-IDF's i2s_std — no ADF. Codec-less: the
 * MAX98357 is a pure-I2S Class-D DAC. The channel runs at ONE FIXED rate
 * (s_rate) and is left ENABLED; the writer task gates it (pause/resume via
 * s_parked / route) — never a runtime reconfig that could wedge the ESP32
 * out-link. The board pins are the old std-mode config. */
static void i2s_writer_task(void *arg)
{
    /* Silence fill lives in s_seam_silence (file scope) and is applied via
     * i2s_seam_rearm() at the seam/park; see that helper. */
    size_t written = 0;
    /* Real underrun counter: incremented ONLY when the writer wanted PCM but
     * the ring was empty while playback was active (the branch below that
     * feeds silence to keep BCLK alive). This is the honest audible-gap
     * signal. The old producer-side "I2S write gap" merely timed two
     * hw_audio_write_pcm() calls (SD read + decode) and false-alarmed on a
     * slow-but-adequate 4 KB read on 48 kHz tracks. */
    static uint32_t s_underruns = 0;
    static int64_t  s_underrun_log_us = 0;
    for (;;) {
        if (s_flush_req) {
            /* Hard switch: fade the queued previous-track PCM to zero on its
             * way to the DMA, so the cut into the mandatory silence gap is a
             * smooth ramp instead of a jump (anti-click, see FADE_OUT_MS).
             * Chunk to one DMA descriptor so i2s_write_bounded cannot time out;
             * frames past the 20 ms window get gain 0 = digital silence. */
            if (s_flush_fadeout) {
                size_t fade_frames = 0;
                while (fade_frames < FADE_OUT_FRAMES) {
                    size_t f_len = 0;
                    uint8_t *f_it = (uint8_t *)xRingbufferReceiveUpTo(
                                        s_pcm_rb, &f_len, 0, PCM_WRITER_CHUNK_MAX);
                    if (f_it == NULL) {
                        break;
                    }
                    taskENTER_CRITICAL(&s_ring_lock);
                    if (f_len <= s_ring_bytes) {
                        s_ring_bytes -= f_len;
                    } else {
                        s_ring_bytes = 0;
                    }
                    taskEXIT_CRITICAL(&s_ring_lock);
                    int16_t *p = (int16_t *)f_it;
                    size_t n = f_len / 2;   /* int16 samples = 2 bytes each */
                    size_t keep = (FADE_OUT_FRAMES - fade_frames) * 2;
                    if (keep < n) {
                        n = keep;   /* rest of this item is dropped below */
                    }
                    for (size_t k = 0; k < n; k++) {
                        size_t fr = fade_frames + k / 2;   /* L/R share a frame */
                        int32_t g = s_fade_cos_q15[(fr * FADE_OUT_STEPS) / FADE_OUT_FRAMES];
                        p[k] = (int16_t)(((int32_t)p[k] * g) >> 15);
                    }
                    fade_frames += n / 2;
                    i2s_write_bounded(f_it, n * 2);   /* write only the faded part */
                    vRingbufferReturnItem(s_pcm_rb, f_it);
                }
                ESP_LOGW(TAG, "[AUDIO] seam fade-out: %u frames ramped to zero",
                         (unsigned)fade_frames);
            }
            /* Drop anything the fade did not consume so the next track starts
             * clean. The channel is deliberately NOT disabled/re-enabled here
             * (see the note below); the DMA queue self-clears via auto_clear
             * plus the silence fill. */
            uint8_t *it; size_t len;
            while ((it = (uint8_t *)xRingbufferReceive(s_pcm_rb, &len, 0)) != NULL) {
                vRingbufferReturnItem(s_pcm_rb, it);
            }
            taskENTER_CRITICAL(&s_ring_lock);
            s_ring_bytes = 0;   /* queue dropped: nothing pending */
            taskEXIT_CRITICAL(&s_ring_lock);
            /* Hard track switch (stop->next, manual next/prev): overwrite the
             * whole DMA descriptor ring with silence BEFORE re-enabling the
             * channel, while the DMA is still running. The descriptors AHEAD of
             * the current pointer still hold the previous track's PCM; if we
             * disable/enable first, the re-enabled channel may re-feed those
             * stale descriptors to the DMA and they get played (the "上一首残音")
             * before our silence can overwrite them. Writing the silence while
             * the channel is still ENABLED overwrites every descriptor buffer
             * up front, so a later enable/re-feed can only emit silence.
             * Skipped when the channel is already parked (disabled): the park
             * path below already clears the ring with silence before disabling,
             * so there is nothing stale to overwrite here. */
            if (s_flush_fill && s_i2s_chan != NULL && !s_i2s_disabled) {
                /* Hard track switch / natural-advance re-arm: reset the TX
                 * out-link so the next track's PCM is actually picked up. */
                i2s_seam_rearm();
            }
            /* i2s_seam_rearm() first floods the ENABLED ring with silence (so
             * no stale descriptor survives) and then does disable()+enable()
             * to restart the out-link (a drain-only seam left track2 dead on
             * this build). Both are required: ESP-IDF documents disable() as
             * only stopping BCLK/WS, so a re-arm alone replays old PCM. See
             * i2s_seam_rearm() for details. */
            s_flush_req = false;
            s_flush_fill = false;
            s_flush_fadeout = false;
            int64_t flush_done_us = esp_timer_get_time();
            int64_t last_dma_us;
            taskENTER_CRITICAL(&s_ring_lock);
            s_last_flush_done_us = flush_done_us;
            last_dma_us = s_last_real_dma_us;
            taskEXIT_CRITICAL(&s_ring_lock);
            ESP_LOGW(TAG,
                     "[SEAMDBG] flush done: active=%d parked=%d route=%d dis=%d "
                     "ring=%u w=%u sil=%u drop=%u post=%d last_dma_age=%lld ms",
                     (int)s_player_active, (int)s_parked, (int)s_route,
                     (int)s_i2s_disabled, (unsigned)s_ring_bytes,
                     (unsigned)s_w_chunks, (unsigned)s_sil_chunks,
                     (unsigned)s_drop_inactive, (int)s_post_peak,
                     (long long)(last_dma_us
                                 ? (flush_done_us - last_dma_us) / 1000
                                 : -1));
            if (s_flush_done != NULL) {
                xSemaphoreGive(s_flush_done);   /* unblock the waiting caller */
            }
            if (s_diag_arm) {
                ESP_LOGW(TAG,
                         "[SEAMDIAG] pre: active=%d parked=%d route=%d dis=%d "
                         "ring=%u w=%u sil=%u drop=%u post=%d",
                         (int)s_player_active, (int)s_parked, (int)s_route,
                         (int)s_i2s_disabled, (unsigned)s_ring_bytes,
                         (unsigned)s_w_chunks, (unsigned)s_sil_chunks,
                         (unsigned)s_drop_inactive, (int)s_post_peak);
                s_diag_arm = false;
            }
        }
        bool paused = (s_route == AUDIO_ROUTE_BT) || s_parked;
        if (paused) {
            /* Stop/park MUTES at once: drop the queued previous-track PCM
             * instead of playing its tail out, then the silence fill below
             * overwrites the DMA descriptors. A stop therefore leaves no
             * audible tail and nothing for the next track to leak ("上一首残音").
             * Ring bytes are still decremented so s_ring_bytes stays truthful. */
            uint8_t *pit; size_t plen;
            while ((pit = (uint8_t *)xRingbufferReceive(s_pcm_rb, &plen, 0)) != NULL) {
                taskENTER_CRITICAL(&s_ring_lock);
                if (plen <= s_ring_bytes) {
                    s_ring_bytes -= plen;
                } else {
                    s_ring_bytes = 0;
                }
                taskEXIT_CRITICAL(&s_ring_lock);
                vRingbufferReturnItem(s_pcm_rb, pit);   /* dropped, not played */
            }
            vTaskDelay(pdMS_TO_TICKS(I2S_DMA_DRAIN_MS));
            /* Reset + prime the DMA descriptor ring at park. i2s_seam_rearm()
             * does a disable+enable (which re-initialises the descriptor queue on
             * the i2s_std driver) followed by a SHORT silence prime. The pending
             * PCM was already dropped and I2S_DMA_DRAIN_MS waited out the DMA
             * tail above, so no previous-track audio survives to leak as
             * "上一首残音". The channel is then left RUNNING but this task blocks
             * below, so the DMA idles and BCLK stops — the amp powers down. (The
             * old comment warning that disable() "wedges the out-link" referred
             * to the ADF I2S driver, not i2s_std.) */
            i2s_seam_rearm();
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  /* wait for resume / flush */
            continue;
        }
        /* Pull PCM from the ring; on underrun, emit silence to keep BCLK alive. */
        uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(
                            s_pcm_rb, &written, pdMS_TO_TICKS(20),
                            PCM_WRITER_CHUNK_MAX);
        if (item != NULL) {
            int64_t pull_us = esp_timer_get_time();
            taskENTER_CRITICAL(&s_ring_lock);
            s_last_ring_pull_us = pull_us;
            if (written <= s_ring_bytes) {
                s_ring_bytes -= written;
            } else {
                s_ring_bytes = 0;   /* defensive: never underflow */
            }
            taskEXIT_CRITICAL(&s_ring_lock);
            if (!s_player_active) {
                /* Stop/park requested: the queued PCM is already gain-processed,
                 * so lowering the volume gain would NOT mute it. Drop it and emit
                 * silence instead, so a stop is silent at once instead of playing
                 * the previous track's tail out — the "停止先 mute" guarantee. */
                vRingbufferReturnItem(s_pcm_rb, item);
                i2s_write_bounded(s_seam_silence, sizeof(s_seam_silence));
                s_drop_inactive++;
                s_sil_chunks++;
            } else {
                /* On a DMA stall the item is returned to the ring and retried
                 * next loop (self-heal); a wedged link is logged, never frozen. */
                const int16_t *dp = (const int16_t *)item;
                size_t dn = written / 2;
                for (size_t i = 0; i < dn; i++) {
                    int32_t a = dp[i] < 0 ? -dp[i] : dp[i];
                    if (a > s_post_peak) {
                        s_post_peak = a;
                    }
                }
                i2s_write_bounded(item, written);
                vRingbufferReturnItem(s_pcm_rb, item);
                s_w_chunks++;
                int64_t dma_us = esp_timer_get_time();
                taskENTER_CRITICAL(&s_ring_lock);
                s_last_real_dma_us = dma_us;
                s_last_ring_pull_us = dma_us;
                taskEXIT_CRITICAL(&s_ring_lock);
                if (s_diag_first) {
                    ESP_LOGW(TAG,
                             "[SEAMDIAG] first real PCM -> DMA: post_peak=%d "
                             "(writer handed track2 audio to the out-link)",
                             (int)s_post_peak);
                    s_diag_first = false;
                }
            }
        } else {
            /* Ring empty but playback active: the DMA is about to run dry and
             * this silence chunk is exactly what fills the gap — a REAL
             * underrun. (Mute drops and park/seam primes take other branches
             * and are deliberately not counted.) Log the first underrun of
             * an episode with the ring's fill level; at most once per second. */
            if (s_player_active) {
                s_underruns++;
                int64_t now_us = esp_timer_get_time();
                if (now_us - s_underrun_log_us > 1000000) {
                    ESP_LOGW(TAG,
                             "[AUDIO] I2S underrun: ring empty while playing, "
                             "%u silence chunks fed to DMA (real audio gap)",
                             (unsigned)s_underruns);
                    s_underrun_log_us = now_us;
                }
            }
            i2s_write_bounded(s_seam_silence, sizeof(s_seam_silence));
            s_sil_chunks++;
        }
    }
}

static esp_err_t audio_create_channel(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = 1024;
    /* CRITICAL fix for "auto-advance silent / next track dead after a seam":
     * this is the real .auto_clear the architecture comment (top of file) claims
     * is in place but was never actually set. In this IDF (v6.1) auto_clear lives
     * on the *channel* config, not the std config. With it OFF, when the PCM ring
     * drains the ESP32 I2S TX out-link STOPS; the next i2s_channel_write() then
     * blocks forever waiting for a DMA credit that never arrives -> the next track
     * is silent (or, if we tore the channel to recover, I2S0 stays wedged). With it
     * ON, the driver memsets each TX descriptor after send (see i2s_dma_tx_callback
     * in esp_driver_i2s/i2s_common.c), so the out-link keeps looping silence while
     * idle and a new i2s_channel_write() is picked up immediately. No channel
     * stop/start, no DMA wedge, seamless auto-advance. */
    chan_cfg.auto_clear = true;
    esp_err_t rc = i2s_new_channel(&chan_cfg, &s_i2s_chan, NULL);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "[AUDIO] i2s_new_channel failed: %s", esp_err_to_name(rc));
        return rc;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_rate),
        /* MAX98357 is a codec-less Class-D DAC that expects standard Philips
         * I2S. MSB/left-justified shifts every sample by one BCLK -> "滋滋滋"
         * static while real PCM still reaches the DMA (post_peak > 0). */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(16, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_NUM_I2S_BCLK,
            .ws   = PIN_NUM_I2S_LRC,
            .dout = PIN_NUM_I2S_DIN,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    rc = i2s_channel_init_std_mode(s_i2s_chan, &std_cfg);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "[AUDIO] i2s_channel_init_std_mode failed: %s",
                 esp_err_to_name(rc));
        i2s_del_channel(s_i2s_chan);
        s_i2s_chan = NULL;
        return rc;
    }
    rc = i2s_channel_enable(s_i2s_chan);   /* BCLK starts; writer task feeds DMA */
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "[AUDIO] i2s_channel_enable failed: %s", esp_err_to_name(rc));
        i2s_del_channel(s_i2s_chan);
        s_i2s_chan = NULL;
        return rc;
    }
    s_pcm_rb = xRingbufferCreate(PCM_RING_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (s_pcm_rb == NULL) {
        ESP_LOGE(TAG, "[AUDIO] PCM ringbuf alloc failed");
        i2s_del_channel(s_i2s_chan);
        s_i2s_chan = NULL;
        return ESP_FAIL;
    }
    /* Pin to Core 1: this is the final real-time link (PCM ring -> I2S DMA).
     * An unpinned task can be scheduled on Core 0, where the Bluetooth
     * controller+stack run at higher priority and starve it during heavy BT
     * traffic, causing I2S underruns (clicks/dropouts).
     *
     * PRIORITY 8, ABOVE lvgl (7) on this SAME core — this is the fix for the
     * "PCM ring full: N bytes dropped (writer stalled >100 ms)" log and the
     * audible gap while ramping the volume: turning the volume key forced a
     * full LVGL page repaint + synchronous SPI flush at prio 7, which
     * preempted this writer at prio 6 for longer than the ring's ~93 ms of
     * slack, so the decode task's xRingbufferSend() timed out and dropped PCM.
     * ui.h:27-34 records the identical earlier failure with LVGL above the
     * real-time consumer on a shared core; the decode task was moved to Core 0
     * but this writer was left below LVGL on Core 1. Raise it so the DMA feed
     * always wins over a cosmetic redraw. Priority 8 matches the decode task,
     * which lives on Core 0, so the two never contend for the same CPU. */
    BaseType_t tr = xTaskCreatePinnedToCore(i2s_writer_task, "i2s_wr", 4096,
                                            NULL, 8, &s_i2s_task, 1);
    if (tr != pdPASS) {
        ESP_LOGE(TAG, "[AUDIO] i2s writer task create failed");
        vRingbufferDelete(s_pcm_rb);
        s_pcm_rb = NULL;
        i2s_del_channel(s_i2s_chan);
        s_i2s_chan = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void hw_audio_init(void)
{
    if (audio_create_channel() != ESP_OK) {
        return;
    }

    s_flush_done = xSemaphoreCreateBinary();
    if (s_flush_done == NULL) {
        ESP_LOGE(TAG, "[AUDIO] flush semaphore alloc failed");
    }

    audio_set_hpf_coeff(s_rate);      /* default-rate HPF coefficient */
    audio_set_loudness_coeff(s_rate); /* default-rate loudness shelf coeff */
    audio_set_drc_coeff(s_rate);      /* default-rate DRC crossover coeff */
    audio_build_vol_table();          /* precompute the 0.1 dB gain table */
    audio_update_vol_gain();
    s_vol_gain_sm = s_vol_gain;       /* start settled: no fade-in from zero */
    s_loud_boost_sm = s_loud_boost;   /* shelf boost settled too */
    s_track_gain = s_track_gain_sm = 32768;   /* 0 dB per-track gain */
    s_master_gain = s_master_gain_sm = 32768; /* 0 dB master gain, settled */

    s_ready = true;

    ESP_LOGI(TAG, "[AUDIO] I2S ready (i2s_std WRITER, %u Hz fixed)",
             (unsigned)s_rate);

    /* Route starts at the speaker. The Bluetooth link callback (see
     * hw_audio_on_bt_conn_state) flips it on connect/disconnect, so output
     * follows the A2DP link regardless of which UI page is on screen — an
     * auto-connect during local I2S playback switches to Bluetooth at once. */
    s_route = AUDIO_ROUTE_SPEAKER;
    bluetooth_audio_set_conn_state_cb(hw_audio_on_bt_conn_state);
}


/* Bluetooth link callback. Route follows the A2DP link, independent of which UI
 * page is shown:
 *  - connect: take the route to Bluetooth at once, so an auto-connect during
 *    local I2S playback promptly switches to A2DP instead of staying on the
 *    speaker (phone-like "BT takes over output" behavior);
 *  - disconnect (remote power-off / out of range / failed dial-out): return to
 *    the speaker immediately so a local session resumes without waiting for the
 *    user to poll the BT page. */
static void hw_audio_on_bt_conn_state(bool connected)
{
    audio_apply_route(connected ? AUDIO_ROUTE_BT : AUDIO_ROUTE_SPEAKER);
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
    if (audio_route_is_bt()) {
        audio_publish_bt_volume();  /* mirror the new level to the sink */
    }
}

uint8_t hw_audio_get_volume(void)
{
    return s_volume;
}

uint8_t hw_audio_get_volume_steps(void)
{
    return (uint8_t)vol_active_max_db();
}

uint8_t hw_audio_get_volume_step(void)
{
    const float max_db = vol_active_max_db();
    return (uint8_t)((float)s_volume * max_db / 100.0f + 0.5f);
}

/* Fixed-dB volume step on the ACTIVE route. The percent↔gain map is dB-linear
 * (see audio_update_vol_gain: dB = (v/100 - 1) * route depth), so stepping
 * by a constant dB is a constant perceived-loudness change everywhere on the
 * knob — no position-dependent "fine near silence / coarse above" patch needed.
 * Convert percent→dB, apply the step, clamp to the route's ends, then round
 * back to the nearest integer percent. NOTE: on the BT route the depth is only
 * 12 dB, so a 1 dB step is ~8 % of the percent knob (the loudness change is
 * still 1 dB; the readout simply moves in bigger jumps there). */
void hw_audio_volume_step_db(float step_db)
{
    int v = (int)s_volume;
    if (v < 0) {
        v = 0;
    }
    else if (v > 100) {
        v = 100;
    }
    const float max_db = vol_active_max_db();
    float dB = ((float)v / 100.0f - 1.0f) * max_db;
    dB += step_db;
    if (dB > 0.0f) {
        dB = 0.0f;
    }
    if (dB < -max_db) {
        dB = -max_db;
    }
    int pct = (int)((dB / max_db + 1.0f) * 100.0f + 0.5f);
    if (pct < 0) {
        pct = 0;
    }
    else if (pct > 100) {
        pct = 100;
    }
    hw_audio_set_volume((uint8_t)pct);
}

bool hw_audio_is_playing(void)
{
    return s_player_active && (s_i2s_chan != NULL) && !s_parked && s_route == AUDIO_ROUTE_SPEAKER;
}

/* Bytes still queued in the PCM ring (decoded but not yet pulled by the writer
 * task). The player polls this after a track hits EOF to learn when the last
 * sample has left the ring; the DMA adds a bounded ~280 ms tail on top, which
 * hw_audio_drain_blocking() also waits out. Returns 0 once everything decoded
 * has at least been handed to the DMA. */
size_t hw_audio_pending(void)
{
    return s_ring_bytes;
}

/* Block until every queued sample has actually been played: the PCM ring
 * drained AND the DMA's in-flight tail flushed. Call at a track boundary or
 * before park so the previous track's audio is fully emitted before the next
 * starts (or before the device goes silent). This is what makes "the song has
 * ended" determinable again after the ring-buffer refactor decoupled the
 * decoder's EOF from real playback. Safe to call from the decode task, which
 * is allowed to block at a seam.
 *
 * s_ring_bytes is volatile, so this loop re-reads it every pass and terminates
 * once the writer task has emptied the ring. It is also hard-bounded: a wedged
 * DMA / stalled writer can never spin this task past the 5 s task-WDT window,
 * and we keep feeding the WDT while we wait. */
void hw_audio_drain_blocking(void)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
    while (s_ring_bytes > 0 && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(5));
#if defined(CONFIG_ESP_TASK_WDT_EN)
        esp_task_wdt_reset();   /* this task must keep the WDT fed while blocked */
#endif
    }
    vTaskDelay(pdMS_TO_TICKS(I2S_DMA_DRAIN_MS));
#if defined(CONFIG_ESP_TASK_WDT_EN)
    esp_task_wdt_reset();
#endif
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
        audio_publish_bt_volume();  /* mirror the new level to the sink */
    }
}

uint8_t hw_audio_get_bt_volume(void)
{
    return s_vol_bt;
}

/* AVRCP absolute volume (0..127, full remote scale): each remote step is an
 * equal ~0.09 dB (12/127) change, resolved through the BT route's 0.1 dB gain
 * table, so the remote and the buttons share one consistent taper. Writes the
 * BT route's slot (the remote is a Bluetooth peer); the live gain and the
 * percent view follow only while the BT route is active. On the speaker route
 * the value is stored for the next route switch — a remote command must never
 * hijack the speaker output, and no sink echo is sent either (the sink is
 * already at that value). */
void hw_audio_set_avrc_volume(uint8_t volume_0_127)
{
    if (volume_0_127 > 127) {
        volume_0_127 = 127;
    }
    s_vol_bt = (uint8_t)(((uint32_t)volume_0_127 * 100u + 63) / 127u);
    if (!audio_route_is_bt()) {
        ESP_LOGI(TAG, "AVRCP volume %u%% stored for BT route (speaker active)",
                 (unsigned)s_vol_bt);
        return;
    }
    s_volume = s_vol_bt;
    if (volume_0_127 == 0) {
        s_vol_gain = 0;                /* hard mute */
        audio_update_loudness_boost();
        return;
    }
    float dB = ((float)volume_0_127 / 127.0f - 1.0f) * (float)VOL_BT_MAX_ATTEN_DB;
    s_vol_gain = s_vol_tab_bt[vol_tab_index(dB, VOL_BT_MAX_ATTEN_DB)];
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
        bluetooth_audio_set_sample_rate(sample_rate_hz);
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
        /* Speex may return a partially built state with err != 0. Destroy it
         * through resamp_free() instead of dropping the pointer on the floor,
         * so repeated rate switches cannot leak resampler states. */
        ESP_LOGE(TAG, "speex resampler init failed (rate %u, err %d: %s)",
                 (unsigned)sample_rate_hz, err, speex_resampler_strerror(err));
        resamp_free();             /* fallback: bypass (wrong pitch, no crash) */
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
 * Call from the task that owns PCM writes. */
void hw_audio_pipeline_flush(void)
{
    if (s_route == AUDIO_ROUTE_BT) {
        bluetooth_audio_flush_pcm_ring();
        return;
    }
    /* Speaker route: ask the writer task to drop the queued PCM AND re-arm the
     * DMA out-link by flooding the descriptor ring with silence (s_flush_fill).
     *
     * auto_clear alone is NOT enough on this IDF build: when the PCM ring
     * drains at a track seam the TX out-link desyncs and the next track's
     * i2s_channel_write() is never picked up -> the second song is silent while
     * the decode side keeps reporting ok (the ring still drains). Flooding
     * fresh silence descriptors re-links the out-link so track2 is picked up.
     * This is the same re-arm hw_audio_pipeline_switch() does for manual next;
     * natural advance used to be a "soft" flush (no re-arm) for a gapless seam,
     * but the desync left the second track dead, so we now pay the ~278 ms
     * silence gap at every seam to guarantee it plays. (Shrinkable later once
     * the exact desync window is measured.)
     *
     * Synchronous: block until the writer has actually dropped the ring and
     * re-armed the DMA. A mere flag + notify is not enough — the caller
     * (decode loop) then immediately opens a NEW track and writes its first
     * PCM; without waiting, that first chunk could land in the ring and be
     * discarded by the still-pending flush, cutting the new song's head.
     * With the full 48 KB silence overwrite back in place the writer is busy
     * ~280 ms at the seam, so this wait must match hw_audio_pipeline_switch()'s
     * 400 ms: a shorter wait lets the decode loop start the new track while
     * the writer is still feeding silence, overflowing the PCM ring and
     * dropping the new song's first samples (commit 9db3895). Still bounded
     * (400 ms) so a wedged writer can never deadlock the caller. */
    if (s_i2s_task == NULL) {
        return;   /* no writer to flush (init failed / not yet up) */
    }
    s_flush_fill = true;   /* re-arm the out-link at the seam (see above) */
    s_flush_req = true;
    xTaskNotifyGive(s_i2s_task);   /* wake the writer if it is parked */
    if (s_flush_done != NULL) {
        xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(400));
    }
}

/* Hard track switch: like hw_audio_pipeline_flush() but the writer also
 * overwrites the DMA descriptor ring with silence so any previous track's
 * PCM still queued in descriptors ahead of the DMA pointer is discarded. Use
 * this on a real switch (stop->next, manual next/prev) where residual audio
 * must not bleed into the next song. Synchronous. */
void hw_audio_pipeline_switch(void)
{
    if (s_route == AUDIO_ROUTE_BT) {
        bluetooth_audio_flush_pcm_ring();
        return;
    }
    if (s_i2s_task == NULL) {
        return;   /* no writer to flush (init failed / not yet up) */
    }
    s_flush_fill = true;
    s_flush_fadeout = true;   /* fade queued old PCM to zero before the seam */
    s_flush_req = true;
    xTaskNotifyGive(s_i2s_task);   /* wake the writer if it is parked */
    if (s_flush_done != NULL) {
        /* Fade (~20 ms of queued PCM) + full-ring silence fill (~272 ms) +
         * disable/enable must all finish before the caller opens the next
         * track, otherwise its first PCM lands in the ring mid-flush and is
         * dropped. 400 ms covers all three. */
        xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(400));
    }
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
 * Without Bluetooth, the speaker path applies the protection high-pass, the
 * master volume, the loudness bass shelf, the per-track ReplayGain, the user
 * master gain and the two-band DRC (400 Hz split, soft-knee compression in
 * each band) with a hard ceiling at the DAC input. The Bluetooth path applies the
 * per-track gain, the user master gain and the master volume only
 * (headphones reproduce full band, and the sink's own
 * limiting handles hot peaks).
 *
 * Speaker output goes through the PCM ring buffer: hw_audio_write_pcm() pushes
 * DSP-produced PCM into it via a bounded xRingbufferSend, which paces the
 * decoder under back-pressure, and the dedicated DMA writer task pops from it
 * and feeds the i2s_std channel. The ring + DMA (12 x 1024 frames, auto_clear)
 * absorb decode jitter. A wedged DMA (ring never drained) surfaces as
 * AUDIO_WRITE_STALLED; a stop/pause mid-write surfaces as
 * AUDIO_WRITE_ABANDONED. */
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
        /* 路由到 BT 时,扬声器 i2s_std 频道已由 writer task 在 set_route 中
           pause(BCLK 停),这里只做 BT 发送与增益。 */
        /* Bluetooth route: volume only, full band. The blocking send inside
         * bluetooth_audio_write_pcm() paces the decoder. The send is now BOUNDED
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
        bool bt_ok = bluetooth_audio_write_pcm(stereo_frames, frames);
        return bt_ok ? AUDIO_WRITE_OK : AUDIO_WRITE_STALLED;
    }

    /* Speaker route: high-pass -> MASTER VOLUME -> loudness bass shelf
     * -> per-track gain -> user master gain -> soft limiter, all per sample
     * (L = even index, R = odd index).
     *
     * WHY THE VOLUME RUNS BEFORE THE SHELF: the shelf adds back up to +9 dB of
     * low-mid (more the lower the volume is), and it used to run at FULL SCALE
     * — i.e. before the volume attenuation — so its output hit the int16 hard
     * clamp and was CLIPPED. The clip amount therefore depended on the knob
     * position (worst below ~50 %, gone above ~60 %): turning the volume down
     * made the sound grow harsh/fuzzy and turning it up cleaned it up again.
     * Every stage up to the limiter is linear, so moving the volume multiply
     * in front of the shelf leaves the frequency response bit-for-bit
     * identical —
     *     g * (y + b*lp(y))  ==  g*y + b*lp(g*y)
     * — while making g*(1+b) <= 1 for EVERY knob position (b = the shelf
     * boost), so the shelf can no longer push the sum past full scale.
     * Verified against a bit-exact simulation of this chain: identical output
     * RMS, clipping 0.93 % -> 0.00 % of the samples on a hot master. */
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

        /* Master volume (see the block comment above: it must precede the
         * loudness shelf, otherwise the shelf's boost clips the int16 sum). */
        g += ((g_target - g) * VOL_SMOOTH_A_Q15) >> 15;
        y = (int32_t)(((int64_t)y * g) >> 15);   /* logarithmic volume */

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

        /* Per-track ReplayGain (scales the whole signal, so it behaves like a
         * mastering gain; the limiter below still bounds the peaks). 64-bit
         * multiply: the gain can exceed 1.0 (up to +12 dB). */
        s_track_gain_sm += ((s_track_gain - s_track_gain_sm) * VOL_SMOOTH_A_Q15) >> 15;
        y = (int32_t)(((int64_t)y * s_track_gain_sm) >> 15);
        /* Keep integer headroom instead of clipping here: the limiter at the
         * end of this chain is the ONLY place allowed to bound the level. */
        if (y > LIM_HEADROOM) {
            y = LIM_HEADROOM;
        }
        else if (y < -LIM_HEADROOM) {
            y = -LIM_HEADROOM;
        }

        /* User master gain (the user's preamp offset). Same 64-bit
         * multiply: ±12 dB can scale by up to ~4x. */
        s_master_gain_sm += ((s_master_gain - s_master_gain_sm) * VOL_SMOOTH_A_Q15) >> 15;
        y = (int32_t)(((int64_t)y * s_master_gain_sm) >> 15);
        if (y > LIM_HEADROOM) {
            y = LIM_HEADROOM;
        }
        else if (y < -LIM_HEADROOM) {
            y = -LIM_HEADROOM;
        }

        /* Two-band DRC. Crossover is complementary (high = x - low), so
         * low + high reconstructs y exactly before the band gains are
         * applied — no phase cancellation at the 400 Hz split. */
        int32_t lo = s_drc_lp[ch]
                     + (int32_t)(((int64_t)(y - s_drc_lp[ch])
                                  * s_drc_lp_coeff) >> 15);
        s_drc_lp[ch] = lo;
        int32_t hi = y - lo;

        /* Full-table, unrolled DRC (bit-exact vs the old compact+guard loop). */
        int32_t mixed = drc_apply_band(0, lo, ch)
                      + drc_apply_band(1, hi, ch);

        /* Hard ceiling: the final line of defence after the two band gains
         * recombine, so no transient can reach 0 dBFS at the amp input. */
        y = mixed;
        if (y > LOUD_LIMIT_CEIL) {
            y = LOUD_LIMIT_CEIL;
        }
        else if (y < -LOUD_LIMIT_CEIL) {
            y = -LOUD_LIMIT_CEIL;
        }
        dsp_buf[i] = (int16_t)y;
    }
    s_vol_gain_sm = g;

    /* MAX98357 is a mono amp: SD_MODE can select left, right, or (L/2 + R/2).
     * Channel-alternating material therefore sounds silent whenever the
     * selected side is the quiet one. Fold both channels into the same mono
     * sample and feed it to both I2S slots, so the speaker always carries
     * both source channels regardless of SD_MODE wiring. This is the mono mix
     * listed in the MAX98357A datasheet and FFmpeg's standard
     * `pan=mono|c0=0.5*c0+0.5*c1` downmix. L/R are already bounded by
     * LOUD_LIMIT_CEIL, so the sum-then-shift cannot overflow. The BT path
     * above is untouched and remains full stereo. */
    for (size_t i = 0; i < dsp_frames; i++) {
        int32_t m = ((int32_t)dsp_buf[2 * i] + (int32_t)dsp_buf[2 * i + 1]) >> 1;
        dsp_buf[2 * i] = (int16_t)m;
        dsp_buf[2 * i + 1] = (int16_t)m;
    }

    size_t bytes = dsp_frames * 4;

    /* Underrun diagnostics now live in the writer task: it is the only place
     * that actually knows the ring ran dry (see s_underruns there). Timing
     * the gap between producer calls only measured SD read + decode latency
     * and false-alarmed on slow-but-adequate reads, so it was removed. */

    /* 频道已在 init 时 enable;这里只把已重采样+DSP 后的 PCM 写入 ringbuf,
       由独立的 DMA writer 任务喂 I2S。无 enable/预热/reconfig/互斥锁。
       backpressure 由 ringbuf 自然处理。 */
    if (s_i2s_chan == NULL || s_pcm_rb == NULL) {
        return AUDIO_WRITE_STALLED;
    }
    /* Push the DSP-produced PCM into the ring in sub-ring chunks. A single
     * decoded (and resampled) frame can be LARGER than the whole PCM ring:
     * the worst case (8 kHz mono -> upmix + resample to 44.1 kHz) reaches
     * ~30 KB (see RESAMP_MAX_FRAMES), and even a 48 kHz stereo frame is
     * ~8.5 KB against the 16 KB ring. A byte-buffer ringbuf fails the send
     * outright when the item exceeds its capacity, so a whole-frame
     * xRingbufferSend() would NEVER succeed and the decoder would spin
     * forever returning AUDIO_WRITE_STALLED (log prints the first frame,
     * then silence — "press play and it hangs", exactly the reported
     * cold-boot-first-play freeze). Chunking each frame into items smaller
     * than the ring lets the DMA writer task drain between chunks, so any
     * frame size streams. */
    size_t off = 0;
    audio_write_result_t res = AUDIO_WRITE_OK;
    while (off < bytes) {
        size_t chunk = bytes - off;
        if (chunk > 1024) {
            chunk = 1024;   /* well under the 16 KB ring; small DMA writes */
        }
        if (xRingbufferSend(s_pcm_rb, (uint8_t *)dsp_buf + off, chunk,
                           pdMS_TO_TICKS(100)) != pdTRUE) {
            /* 暂停/停止落在写中途:不算错误。 */
            if (!s_player_active) {
                return AUDIO_WRITE_ABANDONED;
            }
            /* The send TIMED OUT: the writer task did not drain the ring for
             * 100 ms, so the samples of this chunk are DROPPED (the loop
             * breaks) — that is an audible gap in the music, not just a
             * back-pressure hiccup. Log the FIRST failure of an episode (the
             * counter is cleared by the next successful write) so a dropout
             * shows up in the console with the amount of lost audio. */
            if (s_wr_errs == 0) {
                ESP_LOGE(TAG, "[ERROR] PCM ring full: %u bytes dropped "
                               "(writer stalled >100 ms) -> audible gap",
                         (unsigned)(bytes - off));
                size_t ring_dbg;
                int64_t now_dbg = esp_timer_get_time();
                int64_t last_pull_us, last_dma_us, last_seam_us, last_wr_dur_us;
                uint32_t wr_fail_dbg, slow_wr_dbg;
                bool parked_dbg, disabled_dbg;
                audio_route_t route_dbg;
                taskENTER_CRITICAL(&s_ring_lock);
                ring_dbg = s_ring_bytes;
                last_pull_us = s_last_ring_pull_us;
                last_dma_us = s_last_real_dma_us;
                last_seam_us = s_last_flush_done_us;
                last_wr_dur_us = s_last_i2s_write_dur_us;
                wr_fail_dbg = s_i2s_write_fails;
                slow_wr_dbg = s_i2s_slow_writes;
                parked_dbg = s_parked;
                disabled_dbg = s_i2s_disabled;
                route_dbg = s_route;
                taskEXIT_CRITICAL(&s_ring_lock);
                ESP_LOGW(TAG,
                         "[RINGFULLDBG] ring=%u/%u parked=%d dis=%d route=%d "
                         "w=%u sil=%u drop=%u pull_age=%lld ms dma_age=%lld ms "
                         "seam_age=%lld ms last_wr_dur=%lld us wr_fail=%u "
                         "slow_wr=%u",
                         (unsigned)ring_dbg, (unsigned)PCM_RING_BYTES,
                         (int)parked_dbg, (int)disabled_dbg, (int)route_dbg,
                         (unsigned)s_w_chunks, (unsigned)s_sil_chunks,
                         (unsigned)s_drop_inactive,
                         (long long)(last_pull_us
                                     ? (now_dbg - last_pull_us) / 1000 : -1),
                         (long long)(last_dma_us
                                     ? (now_dbg - last_dma_us) / 1000 : -1),
                         (long long)(last_seam_us
                                     ? (now_dbg - last_seam_us) / 1000 : -1),
                         (long long)last_wr_dur_us,
                         (unsigned)wr_fail_dbg, (unsigned)slow_wr_dbg);
            }
            s_wr_errs++;
            res = AUDIO_WRITE_STALLED;   /* back-pressure: ring full, retry next frame */
            break;
        }

        off += chunk;
        /* Serialised so the writer task's decrement can never lose this add. */
        taskENTER_CRITICAL(&s_ring_lock);
        s_ring_bytes += chunk;   /* account for queued audio (end-of-track detection) */
        taskEXIT_CRITICAL(&s_ring_lock);
    }
    /* 暂停/停止落在写中途:不算错误。 */
    if (!s_player_active) {
        return AUDIO_WRITE_ABANDONED;
    }
    s_wr_errs = 0;
    return res;
}
