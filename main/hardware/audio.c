/*
 * Hardware layer: I2S audio output driving a MAX98357 Class-D DAC.
 * See hardware/audio.h.
 *
 * Wiring (from board_config.h):
 *   BCLK -> GPIO25, LRC(WS) -> GPIO32, DIN -> GPIO33, no MCLK.
 *
 * The I2S bus is configured as 16-bit STEREO; raw PCM is streamed for MP3
 * playback.
 *
 * MP3 playback uses a decoupled pipeline: the decoder task enqueues decoded
 * PCM frames into a ring buffer (with back-pressure so no samples are ever
 * dropped), and a dedicated high-priority feed task streams them to the I2S
 * DMA continuously. This keeps the DMA fed even when a complex MP3 frame
 * takes longer to decode, which previously caused I2S underruns / crackle.
 */
#define LOG_LOCAL_LEVEL ESP_LOG_INFO    /* keep detailed audio tracing out unless explicitly set to DEBUG at compile time */
#include "board_config.h"
#include "hardware/audio.h"
#include "hardware/bt_audio.h"

#include <math.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

static const char *TAG = "hw_audio";

#define AUDIO_DEFAULT_RATE  44100
#define AUDIO_2PI           6.2831853f

/* Speaker-protection high-pass cutoff (Hz).
 *
 * Current speaker specs:
 *   Resonance F₀  : 820 Hz ~ 860 Hz
 *   Frequency range: 800 Hz ~ 8000 Hz
 * The HPF removes everything below ~700 Hz so no excursion is wasted on
 * frequencies the cone cannot reproduce, protecting it and reducing
 * audible distortion around the resonance peak. */
#define SPEAKER_HPF_FC_HZ   700

/* PCM ring buffer: decouples MP3 decode from I2S output. Sized for >1s of
 * 44.1kHz stereo 16-bit audio so decode jitter / SD stalls are absorbed. */
#define PCM_RING_BYTES      (256 * 1024)

static i2s_chan_handle_t s_tx;
static bool s_ready;
static uint32_t s_rate = AUDIO_DEFAULT_RATE;
static bool s_player_active;             /* MP3 player owns the I2S bus */

/* PCM ring buffer + feed task. */
static uint8_t *s_ring_storage;
static StaticRingbuffer_t s_ring_struct;
static RingbufHandle_t s_pcm_ring;
static TaskHandle_t s_feed_task;
static bool s_feeding;
static uint32_t s_pending_rate;          /* applied serially by the feed task */

/* --- Speaker-protection high-pass filter -------------------------------
 * The on-board driver is a small mid-band voice speaker (usable ~800 Hz..6 kHz,
 * resonance 800..1200 Hz). A 1st-order DC-blocking high-pass is inserted before
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

/* Clear the high-pass history (call when a track starts). */
static void audio_hpf_reset(void)
{
    s_hpf_x1[0] = s_hpf_x1[1] = 0;
    s_hpf_y1[0] = s_hpf_y1[1] = 0;
}

static uint8_t s_volume = 80;            /* master output volume, percent */
static int32_t s_vol_gain;               /* Q15 linear gain actually applied */
static int32_t s_vol_target;             /* Q15 linear gain we are ramping to */

/* Volume smoothing: when the user changes the level mid-playback, jumping the
 * per-sample gain produces a sudden step in the waveform — an audible "pop /
 * click" (the gain itself is constant, so pitch/timbre are unchanged). We
 * instead ramp `s_vol_gain` linearly toward `s_vol_target` over a short window
 * so the gain change is continuous. RAMP_MS is the total transition time; the
 * per-sample increment is recomputed each write_pcm call from the current gap
 * and the number of samples being processed (so it always finishes within the
 * call, even for short bursts). */
#define VOL_RAMP_MS        15
static int32_t s_vol_step;               /* Q15 gain delta applied per sample */

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
 *    70    8192      -12.0
 *    50    3261      -20.0
 *    30     818      -32.0
 *    10      82      -44.0
 *     0       0      -∞          mute
 */
#define VOL_MAX_ATTEN_DB 40   /* total attenuation at v=1 (silence ≈ -40 dB);
                               * every 5% press ≈ 2 dB — clearly audible */
static int32_t s_vol_tab[101];

static void audio_build_vol_table(void)
{
    for (int v = 0; v <= 100; v++) {
        if (v <= 0) {
            s_vol_tab[v] = 0;            /* hard mute */
            continue;
        }
        float dB = ((float)v / 100.0f - 1.0f) * (float)VOL_MAX_ATTEN_DB;
        float g = powf(10.0f, dB / 20.0f);
        int32_t val = (int32_t)(g * 32767.0f + 0.5f);
        if (val > 32767) {
            val = 32767;
        }
        s_vol_tab[v] = val;
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
    /* Sets the target only; s_vol_gain is ramped toward it sample-by-sample in
     * hw_audio_write_pcm() to avoid step-noise. Initialize both at startup. */
    s_vol_target = s_vol_tab[v];
}

/* Snap the applied gain straight to the target (no ramp): used at init and on
 * route/track changes where a smooth transition is neither needed nor wanted. */
static void audio_vol_gain_snap(void)
{
    s_vol_gain = s_vol_target;
    s_vol_step = 0;
}

/* Apply a sample-rate change. Must only be called from the feed task so it is
 * serialized with the I2S writes (reconfig disables the channel). */
static void apply_rate(uint32_t rate)
{
    if (!s_ready || rate == s_rate) {
        return;
    }
    ESP_LOGI(TAG, "reconfig I2S rate %u -> %u", (unsigned)s_rate, (unsigned)rate);
    i2s_channel_disable(s_tx);
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    i2s_channel_reconfig_std_clock(s_tx, &clk);
    esp_err_t err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reconfig enable failed: %s", esp_err_to_name(err));
    }
    s_rate = rate;
    audio_set_hpf_coeff(s_rate);
}

static void audio_feed_task(void *arg)
{
    (void)arg;
    uint8_t *item;
    size_t item_size;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* wait for play start */
        while (s_feeding) {
            if (s_pending_rate != 0) {
                apply_rate(s_pending_rate);
                s_pending_rate = 0;
            }
            item = (uint8_t *)xRingbufferReceive(s_pcm_ring, &item_size,
                                                 pdMS_TO_TICKS(50));
            if (item == NULL) {
                continue;   /* ring empty: DMA underruns (auto_clear -> silence) */
            }
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
            }
            vRingbufferReturnItem(s_pcm_ring, item);
        }
        /* Drain the PCM ring without writing to I2S — this makes stop/pause
         * instantaneous rather than playing out the remaining ~1.5 s buffer. */
        while ((item = (uint8_t *)xRingbufferReceive(s_pcm_ring, &item_size, 0)) != NULL) {
            vRingbufferReturnItem(s_pcm_ring, item);
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
    audio_build_vol_table();          /* precompute the dB-taper gain table */
    audio_update_vol_gain();
    audio_vol_gain_snap();             /* start at the configured level, no ramp */
    xTaskCreate(audio_feed_task, "audio_feed", 4 * 1024, NULL, 6, &s_feed_task);
}

bool hw_audio_ready(void)
{
    return s_ready;
}

void hw_audio_set_volume(uint8_t volume_pct)
{
    if (volume_pct > 100) {
        volume_pct = 100;
    }
    s_volume = volume_pct;
    audio_update_vol_gain();
}

uint8_t hw_audio_get_volume(void)
{
    return s_volume;
}

/* Request an I2S sample-rate change; the feed task applies it (serialized).
 * The Bluetooth pipeline is told too, so it can resample to the fixed
 * 44.1 kHz A2DP stream rate. */
void hw_audio_set_sample_rate(uint32_t sample_rate_hz)
{
    s_pending_rate = sample_rate_hz;
    bt_audio_set_sample_rate(sample_rate_hz);
}

/* Mark/unmark the MP3 player as the owner of the I2S bus. */
void hw_audio_set_player_active(bool active)
{
    s_player_active = active;
    if (active) {
        s_feeding = true;
        s_pending_rate = 0;           /* set by the first decoded frame */
        audio_hpf_reset();            /* fresh filter history per track */
        audio_set_hpf_coeff(s_rate);  /* default-rate coeff until 1st frame */
        ESP_LOGD(TAG, "player active: HPF reset, rate=%u", (unsigned)s_rate);
        if (s_feed_task != NULL) {
            xTaskNotifyGive(s_feed_task);
        }
    }
    else {
        s_feeding = false;            /* feed task drains the ring then idles */
        ESP_LOGD(TAG, "player inactive: draining ring");
    }
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
 * Without Bluetooth, the speaker path applies the protection high-pass plus
 * master volume as before. Back-pressure blocks until there is room (PCM is
 * never dropped); if playback stops meanwhile we abandon the rest. */
void hw_audio_write_pcm(int16_t *stereo_frames, size_t frames)
{
    if (!s_ready || s_pcm_ring == NULL || !s_player_active || frames == 0) {
        return;
    }

    const bool bt_out = bt_audio_is_enabled() && bt_audio_is_connected();
    static bool s_prev_bt_out;
    if (bt_out != s_prev_bt_out) {
        s_prev_bt_out = bt_out;
        audio_hpf_reset();         /* fresh filter history on route switch */
        audio_vol_gain_snap();     /* no ramp across a route change */
        ESP_LOGI(TAG, "audio route -> %s", bt_out ? "bluetooth" : "speaker");
    }

    size_t n = frames * 2;

    /* Prepare the volume ramp: if the applied gain differs from the target,
     * compute a per-sample increment so the gain (and thus `s_vol_gain`) moves
     * from its current value to `s_vol_target` across these `n` samples.
     * VOL_RAMP_MS worth of samples is used as the reference, but the step is
     * clamped so a partial buffer still completes the transition within the
     * call — keeping the change click-free without ever leaving a stale gain. */
    int32_t g = s_vol_gain;
    if (g != s_vol_target) {
        int32_t ramp_samples = (int32_t)((uint64_t)s_rate * VOL_RAMP_MS / 1000u);
        if (ramp_samples < 1) {
            ramp_samples = 1;
        }
        int32_t remaining = (int32_t)n;
        if (remaining > ramp_samples) {
            remaining = ramp_samples;       /* cap ramp length to VOL_RAMP_MS */
        }
        s_vol_step = (s_vol_target - g) / remaining;
        if (s_vol_step == 0) {
            /* gap smaller than 1 LSB over the ramp: jump on the last sample */
            s_vol_step = (s_vol_target > g) ? 1 : -1;
        }
    }
    else {
        s_vol_step = 0;
    }

    if (bt_out) {
        /* Bluetooth route: volume only, full band. The blocking send inside
         * bt_audio_write_pcm() paces the decoder; nothing goes to I2S (the
         * speaker stays silent because its ring simply runs empty). */
        for (size_t i = 0; i < n; i++) {
            stereo_frames[i] = (int16_t)(((int32_t)stereo_frames[i] * g) >> 15);
            if (s_vol_step != 0 && g != s_vol_target) {
                g += s_vol_step;
                if ((s_vol_step > 0 && g >= s_vol_target) ||
                    (s_vol_step < 0 && g <= s_vol_target)) {
                    g = s_vol_target;
                }
            }
        }
        s_vol_gain = g;
        bt_audio_write_pcm(stereo_frames, frames);
        return;
    }
    else {
        /* Speaker route: 1st-order high-pass per channel (L = even index,
         * R = odd index), then the master volume. */
        for (size_t i = 0; i < n; i++) {
            int ch = (int)(i & 1);
            int32_t x = stereo_frames[i];
            int32_t y = (x - s_hpf_x1[ch]) + ((s_hpf_lambda * s_hpf_y1[ch]) >> 15);
            s_hpf_x1[ch] = x;
            s_hpf_y1[ch] = y;
            if (y > 32767) {
                y = 32767;
            }
            else if (y < -32768) {
                y = -32768;
            }
            y = (y * g) >> 15;               /* apply (ramped) logarithmic volume */
            stereo_frames[i] = (int16_t)y;
            if (s_vol_step != 0 && g != s_vol_target) {
                g += s_vol_step;
                if ((s_vol_step > 0 && g >= s_vol_target) ||
                    (s_vol_step < 0 && g <= s_vol_target)) {
                    g = s_vol_target;
                }
            }
        }
        s_vol_gain = g;
    }

    size_t bytes = frames * 4;
    int waits = 0;
    while (xRingbufferSend(s_pcm_ring, stereo_frames, bytes,
                           pdMS_TO_TICKS(50)) != pdPASS) {
        if (!s_player_active) {
            return;
        }
        waits++;
    }
    if (waits > 0) {
        ESP_LOGD(TAG, "pcm back-pressure: waited %d x50ms (frames=%u)", waits,
                 (unsigned)frames);
    }
}

