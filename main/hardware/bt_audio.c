/*
 * Hardware layer: Bluetooth A2DP Source audio output.
 * See hardware/bt_audio.h.
 *
 * Pipeline: decode task -> hw_audio_write_pcm() -> bt_audio_write_pcm() ->
 * PCM ring -> A2DP source data callback -> Bluedroid (SBC encode inside the
 * stack) -> Bluetooth sink. The PCM we receive is already high-pass filtered
 * and volume-scaled, so the listener hears exactly the local mix.
 *
 * Connection flow (we are the SOURCE, so we must dial out):
 *   scan -> devices with a RENDERING class-of-device collected into a list ->
 *   the user picks one in the BLUETOOTH UI page -> esp_a2d_source_connect() ->
 *   CONNECTED -> CHECK_SRC_RDY -> MEDIA START -> data callback pulls PCM.
 */
#include "hardware/bt_audio.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "bt_audio";

static volatile bool s_enabled;      /* user wants BT output (settings) */
static volatile bool s_connected;    /* an A2DP sink is connected */
static volatile bool s_streaming;    /* media channel started */
static volatile bt_pair_state_t s_pair_state;
static volatile uint32_t s_passkey;  /* SSP numeric-comparison code */

#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_A2DP_ENABLE)

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"

/* PCM ring between the decode task (producer) and the A2DP data callback
 * (consumer). With the Bluetooth route the I2S ring is bypassed, so this is
 * the ONLY buffer absorbing decode jitter / SD stalls: 128 KB ~= 740 ms at
 * 44.1 kHz stereo 16-bit. Storage lives in PSRAM (no internal DRAM cost). */
#define BT_PCM_RING_BYTES      (128 * 1024)
#define BT_INQ_LEN             10          /* inquiry duration, 1.28s units */
#define BT_MAX_DEVICES         8           /* scan-list capacity */

/* A2DP/SBC always streams at 44.1 kHz; other input rates are resampled. */
#define BT_STREAM_RATE         44100

static bool s_initialized;
static volatile bool s_discovering;
static RingbufHandle_t s_pcm_ring;
static uint8_t *s_ring_storage;
static StaticRingbuffer_t s_ring_struct;

/* Linear-interpolation resampler state (Q16 fixed point, no FPU needed).
 * s_rs_phase indexes a virtual input stream where index 0 is the last frame
 * of the previous call (s_rs_last_*), so interpolation is continuous across
 * buffer boundaries. */
static uint32_t s_in_rate = BT_STREAM_RATE;
static uint32_t s_rs_step;               /* input advance per output, Q16 */
static uint32_t s_rs_phase;              /* position in virtual input, Q16 */
static int16_t s_rs_last_l, s_rs_last_r; /* previous input frame */
static esp_bd_addr_t s_peer_bda;
static char s_peer_name[BT_DEV_NAME_LEN];

/* Discovered audio sinks (COD carries the RENDERING service bit). The list is
 * appended from the Bluedroid callback task and read by the LVGL task; count
 * is published last so readers never see an uninitialized slot. */
typedef struct {
    esp_bd_addr_t bda;
    char name[BT_DEV_NAME_LEN];
} bt_dev_t;

static bt_dev_t s_devs[BT_MAX_DEVICES];
static volatile int s_dev_count;

/* Extract a human-readable device name from inquiry properties (direct BDNAME
 * or from the EIR blob). Returns false if the peer sent no name. */
static bool bt_prop_get_name(esp_bt_gap_cb_param_t *param, char *out, size_t out_len)
{
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
        if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME && p->len > 0) {
            int n = p->len < (int)out_len - 1 ? p->len : (int)out_len - 1;
            memcpy(out, p->val, n);
            out[n] = '\0';
            return true;
        }
        if (p->type == ESP_BT_GAP_DEV_PROP_EIR && p->val != NULL) {
            uint8_t len = 0;
            uint8_t *name = esp_bt_gap_resolve_eir_data(
                (uint8_t *)p->val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
            if (name == NULL) {
                name = esp_bt_gap_resolve_eir_data(
                    (uint8_t *)p->val, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
            }
            if (name != NULL && len > 0) {
                int n = len < (int)out_len - 1 ? len : (int)out_len - 1;
                memcpy(out, name, n);
                out[n] = '\0';
                return true;
            }
        }
    }
    return false;
}

/* Inquiry result: append every discoverable device to the scan list
 * (deduplicated by address). The RENDERING-only filter was dropped to widen
 * the scan range — the user can still only A2DP-connect to a real audio sink,
 * but now phones/speakers that omit the rendering service bit are visible. */
static void bt_handle_disc_res(esp_bt_gap_cb_param_t *param)
{
    bool accept = false;
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
        if (p->type == ESP_BT_GAP_DEV_PROP_COD) {
            uint32_t cod = *(uint32_t *)p->val;
            accept = esp_bt_gap_is_valid_cod(cod);
        }
    }
    if (!accept) {
        return;
    }

    /* Already listed? A later result may finally carry the name: fill it in. */
    for (int i = 0; i < s_dev_count; i++) {
        if (memcmp(s_devs[i].bda, param->disc_res.bda, sizeof(esp_bd_addr_t)) == 0) {
            if (s_devs[i].name[0] == '\0') {
                bt_prop_get_name(param, s_devs[i].name, sizeof(s_devs[i].name));
            }
            return;
        }
    }
    if (s_dev_count >= BT_MAX_DEVICES) {
        return;
    }

    bt_dev_t *d = &s_devs[s_dev_count];
    memcpy(d->bda, param->disc_res.bda, sizeof(esp_bd_addr_t));
    if (!bt_prop_get_name(param, d->name, sizeof(d->name))) {
        d->name[0] = '\0';                  /* UI falls back to the address */
    }
    s_dev_count++;                          /* publish after the slot is ready */
    ESP_LOGI(TAG, "sink %d: %02x:%02x:%02x:%02x:%02x:%02x '%s'", s_dev_count,
             d->bda[0], d->bda[1], d->bda[2], d->bda[3], d->bda[4], d->bda[5],
             d->name);
}

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT:
        bt_handle_disc_res(param);
        break;
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            s_discovering = false;
            ESP_LOGI(TAG, "scan finished: %d sink(s)", s_dev_count);
        }
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        s_pair_state = (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS)
                       ? BT_PAIR_OK : BT_PAIR_FAIL;
        ESP_LOGI(TAG, "BT auth %s",
                 s_pair_state == BT_PAIR_OK ? "ok" : "fail");
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        /* Legacy pairing: reply with the conventional "1234". */
        esp_bt_pin_code_t pin = {'1', '2', '3', '4'};
        s_passkey = 1234;
        s_pair_state = BT_PAIR_PAIRING;
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
        break;
    }
#if defined(CONFIG_BT_SSP_ENABLED)
    case ESP_BT_GAP_CFM_REQ_EVT:
        /* SSP numeric comparison: expose the passkey to the UI, then confirm
         * (we have no keyboard; the number is shown for the user to verify
         * against the sink if it has a display). */
        s_passkey = param->cfm_req.num_val;
        s_pair_state = BT_PAIR_PAIRING;
        ESP_LOGI(TAG, "SSP confirm, passkey %06u", (unsigned)s_passkey);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        /* Peer will type this passkey on its side; just display it. */
        s_passkey = param->key_notif.passkey;
        s_pair_state = BT_PAIR_PAIRING;
        ESP_LOGI(TAG, "SSP passkey notify %06u", (unsigned)s_passkey);
        break;
#endif
    default:
        break;
    }
}

static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            s_connected = true;
            s_pair_state = BT_PAIR_OK;
            ESP_LOGI(TAG, "A2DP connected, checking media readiness");
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        }
        else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            /* A drop while still CONNECTING means the attempt failed. */
            if (!s_connected && s_pair_state != BT_PAIR_IDLE) {
                s_pair_state = BT_PAIR_FAIL;
            }
            else {
                s_pair_state = BT_PAIR_IDLE;
            }
            s_connected = false;
            s_streaming = false;
            ESP_LOGI(TAG, "A2DP disconnected");
        }
        break;
    case ESP_A2D_AUDIO_STATE_EVT: {
        bool started = (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED);
        if (started && s_pcm_ring != NULL) {
            /* Drop stale PCM from a previous session before streaming. */
            uint8_t *item;
            size_t sz;
            while ((item = xRingbufferReceiveUpTo(s_pcm_ring, &sz, 0,
                                                  BT_PCM_RING_BYTES)) != NULL) {
                vRingbufferReturnItem(s_pcm_ring, item);
            }
        }
        s_streaming = started;
        ESP_LOGI(TAG, "A2DP audio %s", started ? "started" : "stopped");
        break;
    }
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY &&
            param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
        }
        break;
    default:
        break;
    }
}

/* A2DP source data callback: the stack asks for `len` bytes of 44.1 kHz
 * stereo 16-bit PCM (SBC encoding happens inside Bluedroid). Pull from the
 * ring; pad with silence on underrun so the stream never stalls. */
static int32_t a2d_data_cb(uint8_t *buf, int32_t len)
{
    if (buf == NULL || len <= 0) {
        /* len == -1 asks us to flush any queued data. */
        if (len < 0 && s_pcm_ring != NULL) {
            uint8_t *item;
            size_t sz;
            while ((item = xRingbufferReceiveUpTo(s_pcm_ring, &sz, 0,
                                                  BT_PCM_RING_BYTES)) != NULL) {
                vRingbufferReturnItem(s_pcm_ring, item);
            }
        }
        return 0;
    }

    int32_t filled = 0;
    while (filled < len && s_pcm_ring != NULL) {
        size_t sz = 0;
        uint8_t *item = xRingbufferReceiveUpTo(s_pcm_ring, &sz, 0,
                                               (size_t)(len - filled));
        if (item == NULL) {
            break;                          /* ring empty */
        }
        memcpy(buf + filled, item, sz);
        vRingbufferReturnItem(s_pcm_ring, item);
        filled += (int32_t)sz;
    }
    if (filled < len) {
        memset(buf + filled, 0, (size_t)(len - filled));   /* silence pad */
    }
    return len;
}

void bt_audio_init(void)
{
    esp_err_t err;

    /* Only BR/EDR (classic BT) is needed for A2DP; release BLE memory. */
    (void)esp_bt_controller_mem_release(ESP_BT_MODE_BLE);

    esp_bt_controller_config_t ctrl_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&ctrl_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BT controller init: %s", esp_err_to_name(err));
        return;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BT controller enable: %s", esp_err_to_name(err));
        return;
    }
    err = esp_bluedroid_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid init: %s", esp_err_to_name(err));
        return;
    }
    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid enable: %s", esp_err_to_name(err));
        return;
    }

    esp_bt_gap_register_callback(gap_cb);
    esp_bt_gap_set_device_name("Xiaomao MP3");
    /* Source dials out; stay non-discoverable but connectable. */
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    esp_a2d_register_callback(a2d_cb);
    esp_a2d_source_register_data_callback(a2d_data_cb);
    esp_a2d_source_init();

    s_ring_storage = heap_caps_malloc(BT_PCM_RING_BYTES, MALLOC_CAP_SPIRAM);
    if (s_ring_storage == NULL) {
        s_ring_storage = malloc(BT_PCM_RING_BYTES);
    }
    if (s_ring_storage != NULL) {
        s_pcm_ring = xRingbufferCreateStatic(BT_PCM_RING_BYTES,
                                             RINGBUF_TYPE_BYTEBUF,
                                             s_ring_storage, &s_ring_struct);
    }
    if (s_pcm_ring == NULL) {
        ESP_LOGE(TAG, "BT PCM ring alloc failed");
    }
    s_initialized = true;
    ESP_LOGI(TAG, "A2DP source ready ('Xiaomao MP3')");
}

void bt_audio_set_enabled(bool enabled)
{
    s_enabled = enabled;
    if (!enabled && s_connected) {
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_STOP);
        esp_a2d_source_disconnect(s_peer_bda);
    }
}

void bt_audio_scan_start(void)
{
    /* Never scan while linked: inquiry steals RF bandwidth from the A2DP
     * stream and makes playback stutter. */
    if (!s_initialized || s_discovering || s_connected) {
        return;
    }
    s_dev_count = 0;                        /* fresh list per scan */
    esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
                                               BT_INQ_LEN, 0);
    if (err == ESP_OK) {
        s_discovering = true;
        ESP_LOGI(TAG, "scanning for Bluetooth audio sinks...");
    }
    else {
        ESP_LOGW(TAG, "start discovery failed: %s", esp_err_to_name(err));
    }
}

bool bt_audio_is_scanning(void)
{
    return s_discovering;
}

int bt_audio_device_count(void)
{
    return s_dev_count;
}

const char *bt_audio_device_name(int index)
{
    static char fallback[18];
    if (index < 0 || index >= s_dev_count) {
        return "";
    }
    if (s_devs[index].name[0] != '\0') {
        return s_devs[index].name;
    }
    /* Nameless device: show its address instead. */
    snprintf(fallback, sizeof(fallback), "%02X:%02X:%02X:%02X:%02X:%02X",
             s_devs[index].bda[0], s_devs[index].bda[1], s_devs[index].bda[2],
             s_devs[index].bda[3], s_devs[index].bda[4], s_devs[index].bda[5]);
    return fallback;
}

bool bt_audio_connect_index(int index)
{
    if (!s_initialized || index < 0 || index >= s_dev_count) {
        return false;
    }
    if (s_discovering) {
        esp_bt_gap_cancel_discovery();      /* radio can't scan and dial */
    }
    memcpy(s_peer_bda, s_devs[index].bda, sizeof(esp_bd_addr_t));
    snprintf(s_peer_name, sizeof(s_peer_name), "%s", bt_audio_device_name(index));
    s_enabled = true;                       /* connecting implies BT output on */
    s_pair_state = BT_PAIR_CONNECTING;
    esp_err_t err = esp_a2d_source_connect(s_peer_bda);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(err));
        s_pair_state = BT_PAIR_FAIL;
        return false;
    }
    ESP_LOGI(TAG, "connecting to '%s'...", s_peer_name);
    return true;
}

void bt_audio_disconnect(void)
{
    if (s_connected) {
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_STOP);
        esp_a2d_source_disconnect(s_peer_bda);
    }
}

const char *bt_audio_peer_name(void)
{
    return s_peer_name;
}

/* Blocking back-pressure send into the BT ring. The A2DP data callback
 * drains at exactly real time, which paces the decode task. Bail out if
 * streaming stops while waiting. */
static void bt_ring_send(const void *data, size_t bytes)
{
    while (xRingbufferSend(s_pcm_ring, data, bytes,
                           pdMS_TO_TICKS(50)) != pdPASS) {
        if (!s_streaming || !s_enabled) {
            return;
        }
    }
}

void bt_audio_set_sample_rate(uint32_t rate_hz)
{
    if (rate_hz == 0 || rate_hz == s_in_rate) {
        return;
    }
    s_in_rate = rate_hz;
    s_rs_step = (uint32_t)(((uint64_t)rate_hz << 16) / BT_STREAM_RATE);
    s_rs_phase = 0;
    s_rs_last_l = s_rs_last_r = 0;
    ESP_LOGI(TAG, "BT resampler: %u -> %u Hz (step Q16=%u)",
             (unsigned)rate_hz, BT_STREAM_RATE, (unsigned)s_rs_step);
}

void bt_audio_write_pcm(const int16_t *stereo_frames, size_t frames)
{
    if (!s_enabled || !s_streaming || s_pcm_ring == NULL || frames == 0) {
        return;
    }

    /* Track already at the SBC stream rate: pass through untouched. */
    if (s_in_rate == BT_STREAM_RATE) {
        bt_ring_send(stereo_frames, frames * 4);
        return;
    }

    /* Resample to 44.1 kHz with linear interpolation. Virtual input index 0
     * is the previous call's last frame, indices 1..frames map to this
     * buffer, so the output is continuous across calls. Output is staged in
     * a small chunk buffer and flushed with back-pressure. */
    int16_t out[256 * 2];
    size_t out_n = 0;
    while ((s_rs_phase >> 16) < (uint32_t)frames) {
        uint32_t idx = s_rs_phase >> 16;
        int32_t frac = (int32_t)(s_rs_phase & 0xFFFF);
        int32_t al = (idx == 0) ? s_rs_last_l : stereo_frames[2 * (idx - 1)];
        int32_t ar = (idx == 0) ? s_rs_last_r : stereo_frames[2 * (idx - 1) + 1];
        int32_t bl = stereo_frames[2 * idx];
        int32_t br = stereo_frames[2 * idx + 1];
        out[2 * out_n]     = (int16_t)(al + (((bl - al) * frac) >> 16));
        out[2 * out_n + 1] = (int16_t)(ar + (((br - ar) * frac) >> 16));
        if (++out_n == 256) {
            bt_ring_send(out, sizeof(out));
            out_n = 0;
        }
        s_rs_phase += s_rs_step;
    }
    if (out_n > 0) {
        bt_ring_send(out, out_n * 4);
    }
    s_rs_phase -= (uint32_t)frames << 16;   /* carry fraction to next call */
    s_rs_last_l = stereo_frames[2 * (frames - 1)];
    s_rs_last_r = stereo_frames[2 * (frames - 1) + 1];
}

#else /* Bluetooth / A2DP not compiled in */

void bt_audio_init(void)
{
    ESP_LOGW(TAG, "Bluetooth/A2DP not enabled in build (see sdkconfig)");
}

void bt_audio_set_enabled(bool enabled)
{
    (void)enabled;                          /* stays off without BT support */
}

void bt_audio_write_pcm(const int16_t *stereo_frames, size_t frames)
{
    (void)stereo_frames;
    (void)frames;
}

void bt_audio_set_sample_rate(uint32_t rate_hz)
{
    (void)rate_hz;
}

void bt_audio_scan_start(void)
{
}

bool bt_audio_is_scanning(void)
{
    return false;
}

int bt_audio_device_count(void)
{
    return 0;
}

const char *bt_audio_device_name(int index)
{
    (void)index;
    return "";
}

bool bt_audio_connect_index(int index)
{
    (void)index;
    return false;
}

void bt_audio_disconnect(void)
{
}

const char *bt_audio_peer_name(void)
{
    return "";
}

#endif /* CONFIG_BT_ENABLED && CONFIG_BT_A2DP_ENABLE */

bt_pair_state_t bt_audio_pair_state(void)
{
    return s_pair_state;
}

uint32_t bt_audio_passkey(void)
{
    return s_passkey;
}

bool bt_audio_is_enabled(void)
{
    return s_enabled;
}

bool bt_audio_is_connected(void)
{
    return s_connected;
}
