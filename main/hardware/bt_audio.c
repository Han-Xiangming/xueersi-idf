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
#include "freertos/timers.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "bt_audio";

static volatile bool s_enabled;      /* user wants BT output (settings) */
static volatile bool s_connected;    /* an A2DP sink is connected */
static volatile bool s_streaming;    /* media channel started */
static volatile bt_pair_state_t s_pair_state;
static volatile uint32_t s_passkey;  /* SSP numeric-comparison code */

/* AVRCP remote-control handlers registered by the application (see below).
 * Kept outside the #if so bt_audio_set_avrc_*_cb() compiles even when Bluetooth
 * is disabled in the build 鈥?the handlers are simply never invoked then. */
static bt_avrc_cmd_cb_t s_avrc_cmd_cb;
static bt_avrc_volume_cb_t s_avrc_vol_cb;

#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_A2DP_ENABLE)

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

/* PCM ring between the decode task (producer) and the A2DP data callback
 * (consumer). With the Bluetooth route the I2S ring is bypassed, so this is
 * the ONLY buffer absorbing decode jitter / SD stalls: 128 KB ~= 740 ms at
 * 44.1 kHz stereo 16-bit. Storage lives in PSRAM (no internal DRAM cost). */
#define BT_PCM_RING_BYTES      (128 * 1024)
#define BT_INQ_LEN             10          /* inquiry duration, 1.28s units */

/* Teardown safety. bt_audio_disable() defers the real stack deinit to the
 * FreeRTOS Timer task (bt_audio_teardown): deinit'ing A2DP while Bluedroid
 * still has an armed media watchdog alarm (or while the link is still up)
 * crashes the BTC task. The teardown first asks the link to drop, then polls
 * s_connected with a bounded retry budget; the safety window covers a lost
 * disconnect-complete event so a stuck link cannot leave the stack half
 * disabled forever. */
#define BT_TD_RECHECK_MS       (50)         /* polling interval */
#define BT_TD_MAX_RECHECK      (20)         /* 1 s of polling for the link */
#define BT_TD_SAFETY_MS        (4000)       /* force teardown fallback */

/* A2DP/SBC always streams at 44.1 kHz; other input rates are resampled. */
#define BT_STREAM_RATE         44100

static bool s_initialized;
static volatile bool s_discovering;
/* Set while a teardown is pending across an in-flight link; the actual
 * esp_a2d_source_deinit() runs from the disconnect-complete event, never
 * inline, so the still-armed media watchdog alarm cannot fire into freed
 * control blocks (would crash the BTC task). */
static volatile bool s_disabling;
/* Frozen while a teardown is in progress: the A2DP data callback must not
 * pull more PCM (and the decode side must not push more) once the profiles
 * start going away 鈥?feeding SBC during A2DP deinit can crash the BTC task. */
static volatile bool s_tx_stopped;
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
/* Bumped whenever the discovered-device list changes (a device added, or a
 * nameless device's name filled in) so consumers can cheaply detect "list
 * changed" without re-scanning/formatting the list every tick. */
static uint32_t s_dev_version;

/* Connection auto-retry. A fresh A2DP dial-out can fail the first attempt(s)
 * because Bluedroid opens the AVDTP L2CAP channel before the peer's feature
 * mask has been read back (the "remote features unknown" race) 鈥?or because we
 * dial out while the inquiry is still cancelling. Both clear up if we just try
 * again after a short backoff, so we retry a bounded number of times instead of
 * making the user hammer the A button. The retry runs from a FreeRTOS timer
 * (not the BTC task), which is a safe context for esp_a2d_* calls. */
#define BT_CONNECT_RETRY_MS   (2000)   /* backoff between auto retries */
#define BT_CONNECT_MAX_RETRY  (4)      /* give up after this many failures */
static TimerHandle_t s_conn_timer;
static bool          s_conn_auto;      /* user still wants this link up */
static uint8_t       s_conn_retries;

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
 * the scan range 鈥?the user can still only A2DP-connect to a real audio sink,
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
                s_dev_version++;          /* list contents changed */
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
    s_dev_version++;                        /* list changed: UI must re-read */
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

/* Forward declaration. The real teardown runs in the FreeRTOS Timer task
 * (deferred from the BTC callback) so the esp_*_deinit() calls never execute
 * inside the BTC task itself 鈥?doing that there would deadlock. The Timer-task
 * context also lets Bluedroid finish disarming the media watchdog alarm first. */
static void bt_audio_teardown(void *param1, uint32_t param2);

static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            s_connected = true;
            s_pair_state = BT_PAIR_OK;
            s_conn_auto = false;             /* link is up: stop auto-retry */
            s_conn_retries = 0;
            if (s_disabling) {
                /* bt_audio_disable() raced with the connect completing:
                 * drop the fresh link so the disconnect-complete event can
                 * run the deferred teardown. */
                esp_a2d_source_disconnect(s_peer_bda);
                break;
            }
            ESP_LOGI(TAG, "A2DP connected, checking media readiness");
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        }
        else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            if (!s_connected && s_pair_state != BT_PAIR_IDLE) {
                /* The attempt never reached CONNECTED, i.e. it failed. If
                 * auto-retry is armed and we have attempts left, schedule
                 * another dial-out after the backoff; otherwise surface the
                 * failure to the UI ("A閲嶈瘯"). */
                if (s_conn_auto && s_conn_retries < BT_CONNECT_MAX_RETRY
                    && s_conn_timer != NULL) {
                    xTimerStart(s_conn_timer, 0);   /* backoff then retry */
                }
                else {
                    s_conn_auto = false;
                    s_pair_state = BT_PAIR_FAIL;
                }
            }
            else {
                /* A live link dropped (user or remote). Don't auto-retry a
                 * session that was already up 鈥?let the user decide. */
                s_pair_state = BT_PAIR_IDLE;
                s_conn_auto = false;
            }
            s_connected = false;
            s_streaming = false;
            ESP_LOGI(TAG, "A2DP disconnected");
            if (s_disabling) {
                /* The link is gone and Bluedroid has disarmed the media
                 * watchdog alarm. Defer the actual deinit to the Timer task:
                 * running esp_*_deinit() from inside this BTC callback would
                 * post to the BTC queue and deadlock. */
                xTimerPendFunctionCall(bt_audio_teardown,
                                       NULL, 0, pdMS_TO_TICKS(10));
            }
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

/* AVRCP Target callback: a connected CT (headset/speaker media keys) sends
 * passthrough commands and absolute volume here. Bluedroid auto-ACKs those, so
 * we only act on them. Only the PRESSED key-state is interesting (RELEASED is
 * the auto-repeat tail and is ignored). */
static void avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "AVRCP %sconnected",
                 param->conn_stat.connected ? "" : "dis");
        break;
    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
        if (param->psth_cmd.key_state != ESP_AVRC_PT_CMD_STATE_PRESSED) {
            break;                              /* ignore key-release events */
        }
        if (s_avrc_cmd_cb != NULL) {
            switch ((esp_avrc_pt_cmd_t)param->psth_cmd.key_code) {
            case ESP_AVRC_PT_CMD_PLAY:
                s_avrc_cmd_cb(BT_AVRC_CMD_PLAY);
                break;
            case ESP_AVRC_PT_CMD_PAUSE:
                s_avrc_cmd_cb(BT_AVRC_CMD_PAUSE);
                break;
            case ESP_AVRC_PT_CMD_STOP:
                s_avrc_cmd_cb(BT_AVRC_CMD_STOP);
                break;
            case ESP_AVRC_PT_CMD_FORWARD:
            case ESP_AVRC_PT_CMD_FAST_FORWARD:
                s_avrc_cmd_cb(BT_AVRC_CMD_NEXT);
                break;
            case ESP_AVRC_PT_CMD_BACKWARD:
            case ESP_AVRC_PT_CMD_REWIND:
                s_avrc_cmd_cb(BT_AVRC_CMD_PREV);
                break;
            default:
                break;
            }
        }
        break;
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
        if (s_avrc_vol_cb != NULL) {
            s_avrc_vol_cb(param->set_abs_vol.volume);   /* 0..127 */
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
    if (s_tx_stopped) {
        /* Teardown in progress: refuse to feed the encoder. Returning 0 lets
         * Bluedroid run its own silence-fill path, which is safe; feeding it
         * from our ring while the stack is half-freed is not. */
        return 0;
    }
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
    /* At boot we only allocate the PCM ring (cheap, no BT controller needed).
     * The Bluetooth controller / Bluedroid stack / A2DP source are brought up
     * lazily by bt_audio_enable() the first time the user opens the BLUETOOTH
     * page, so the device stays silent at boot instead of advertising an A2DP
     * source before anyone asks for Bluetooth. */
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
    ESP_LOGI(TAG, "BT audio ring ready (stack deferred until BLUETOOTH page)");
}

/* Bring up the Bluetooth controller, Bluedroid stack and A2DP Source role.
 * Safe to call repeatedly (idempotent); the first call does the real work.
 * Call when the user opens the BLUETOOTH page, not at boot. */
void bt_audio_enable(void)
{
    if (s_initialized) {
        /* Already up. Clear any teardown that bt_audio_disable() deferred
         * while a link was dropping: the user flipped BT back ON before the
         * deferred teardown ran, so we keep the stack and just drop the link. */
        s_disabling = false;
        s_tx_stopped = false;
        return;
    }
    s_disabling = false;

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

    /* Register the AVRCP Target role so a paired headset/speaker can drive
     * local playback and volume with its media keys. */
    esp_avrc_tg_register_callback(avrc_tg_cb);
    esp_avrc_tg_init();

    esp_a2d_register_callback(a2d_cb);
    esp_a2d_source_register_data_callback(a2d_data_cb);
    esp_a2d_source_init();

    s_initialized = true;
    ESP_LOGI(TAG, "A2DP source ready ('Xiaomao MP3')");
}

/* Perform the actual stack teardown and controller power-off. Runs in the
 * FreeRTOS Timer task (see a2d_cb). MUST only be entered when no A2DP link is
 * live: while streaming, Bluedroid arms a media watchdog alarm, and deinit()-ing
 * the source profile before that alarm fires (or is disarmed) makes it run into
 * freed control blocks 鈥?a NULL-deref crash in the BTC task. Re-checks
 * s_disabling so a concurrent bt_audio_enable() can cancel a pending teardown
 * (the user flipped BT back ON before it ran). */
static void bt_audio_teardown(void *param1, uint32_t param2)
{
    (void)param1;
    (void)param2;
    if (!s_disabling) {
        return;                             /* re-enabled: keep the stack up */
    }
    /* Freeze the PCM feed first: while the link is going away the stack may
     * still pull data, and feeding SBC during profile teardown can crash the
     * BTC task. */
    s_tx_stopped = true;
    if (s_connected) {
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
        esp_a2d_source_disconnect(s_peer_bda);
    }
    /* Wait (bounded) for the link to actually drop so deinit never runs
     * while Bluedroid still has an armed media watchdog alarm. The bounded
     * retry budget means a stuck link cannot hold the teardown forever. */
    int waited = 0;
    while (s_connected && waited < BT_TD_MAX_RECHECK) {
        vTaskDelay(pdMS_TO_TICKS(BT_TD_RECHECK_MS));
        waited++;
    }
    if (s_connected) {
        ESP_LOGW(TAG, "link still up after %d ms of teardown polling, "
                      "tearing down anyway", waited * BT_TD_RECHECK_MS);
    }
    /* Bluedroid requires the AVRCP Target to be torn down *before* the A2DP
     * source; deinit'ing A2DP first triggers a "AVRC TG should deinit in
     * advance of A2DP" warning and can leave the AVRC profile half-freed. */
    esp_avrc_tg_deinit();
    esp_a2d_source_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();

    /* Reset all state so the next bt_audio_enable() starts from scratch. */
    s_initialized = false;
    s_connected   = false;
    s_streaming   = false;
    s_enabled     = false;
    s_discovering = false;
    s_dev_count   = 0;
    s_dev_version++;                        /* list reset: UI must re-read */
    s_pair_state  = BT_PAIR_IDLE;
    s_disabling   = false;
    s_tx_stopped  = false;
    s_conn_auto   = false;
    s_conn_retries = 0;
    if (s_conn_timer != NULL) {
        xTimerStop(s_conn_timer, 0);
    }
    s_peer_name[0] = '\0';

    ESP_LOGI(TAG, "BT audio disabled, controller powered off");
}

/* Tear the stack down and power off the controller 鈥?the reverse of
 * bt_audio_enable(). Idempotent: returns immediately if already down or a
 * teardown is already pending. The real work is deferred to the disconnect
 * event when a link is live (see bt_audio_teardown for why). */
void bt_audio_disable(void)
{
    if (!s_initialized || s_disabling) {
        return;
    }

    if (s_connected || s_pair_state == BT_PAIR_CONNECTING) {
        /* A live (or in-flight) link: stop the media stream and kick a
         * disconnect, then defer the teardown to the Timer task 鈥?it re-checks
         * the link with a bounded retry budget (see bt_audio_teardown) instead
         * of relying on the disconnect-complete event alone. The second
         * pending call is the safety net: if that event is lost, the teardown
         * still runs after BT_TD_SAFETY_MS, polls for the link to drop, and
         * proceeds 鈥?the stack cannot stay half-disabled forever. Doing it
         * inline here is what crashes. */
        s_disabling = true;
        s_tx_stopped = true;
        if (s_streaming) {
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
        }
        esp_a2d_source_disconnect(s_peer_bda);
        xTimerPendFunctionCall(bt_audio_teardown, NULL, 0, 0);
        xTimerPendFunctionCall(bt_audio_teardown, NULL, 0,
                               pdMS_TO_TICKS(BT_TD_SAFETY_MS));
        ESP_LOGI(TAG, "BT disable pending disconnect");
        return;
    }

    /* No active link: the alarm is not armed, so teardown inline (UI task
     * context, safe) is fine. Set s_disabling first so the re-check passes. */
    s_disabling = true;
    bt_audio_teardown(NULL, 0);
}

void bt_audio_set_enabled(bool enabled)
{
    s_enabled = enabled;
    if (!enabled) {
        /* BT output turned off: stop any in-flight auto-retry. */
        s_conn_auto = false;
        if (s_conn_timer != NULL) {
            xTimerStop(s_conn_timer, 0);
        }
        if (s_connected) {
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
            esp_a2d_source_disconnect(s_peer_bda);
        }
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
    s_dev_version++;                        /* force the UI to clear its cache */
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

uint32_t bt_audio_device_version(void)
{
    return s_dev_version;
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

/* (Re)issue the A2DP dial-out to s_peer_bda. Returns false only if the stack
 * could not queue the request; the async result arrives via a2d_cb. Callers
 * must be single-flight 鈥?do not call while BT_PAIR_CONNECTING. */
static bool bt_conn_start(void)
{
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

/* FreeRTOS timer callback: re-dial the last peer after a failed attempt,
 * bounded by BT_CONNECT_MAX_RETRY. Runs off the BTC task, so esp_a2d_*
 * calls here are safe. */
static void bt_conn_retry_cb(TimerHandle_t t)
{
    (void)t;
    if (!s_conn_auto || s_connected || s_disabling || !s_initialized) {
        return;                             /* linked, disabled, or gone */
    }
    if (s_pair_state == BT_PAIR_CONNECTING) {
        /* Previous attempt still in flight; nudge again after the backoff. */
        xTimerStart(s_conn_timer, 0);
        return;
    }
    if (s_conn_retries >= BT_CONNECT_MAX_RETRY) {
        s_conn_auto = false;
        s_pair_state = BT_PAIR_FAIL;        /* hand back to UI "A閲嶈瘯" */
        return;
    }
    s_conn_retries++;
    ESP_LOGI(TAG, "retrying connect (%d/%d)...",
             s_conn_retries, BT_CONNECT_MAX_RETRY);
    bt_conn_start();
}

bool bt_audio_connect_index(int index)
{
    if (!s_initialized || index < 0 || index >= s_dev_count) {
        return false;
    }
    /* Single-flight: an attempt already in progress means the auto-retry (or a
     * prior press) owns the link. Report "connecting" instead of spawning a
     * second overlapping dial-out 鈥?overlapping attempts are exactly what makes
     * the "remote features unknown" failure repeat. */
    if (s_pair_state == BT_PAIR_CONNECTING) {
        return true;
    }
    if (s_discovering) {
        esp_bt_gap_cancel_discovery();      /* radio can't scan and dial */
    }
    memcpy(s_peer_bda, s_devs[index].bda, sizeof(esp_bd_addr_t));
    snprintf(s_peer_name, sizeof(s_peer_name), "%s", bt_audio_device_name(index));
    s_enabled = true;                       /* connecting implies BT output on */
    s_conn_auto = true;                     /* keep auto-retrying until linked */
    s_conn_retries = 0;
    if (s_conn_timer == NULL) {
        s_conn_timer = xTimerCreate("bt_conn",
                                    pdMS_TO_TICKS(BT_CONNECT_RETRY_MS),
                                    pdFALSE, NULL, bt_conn_retry_cb);
    }
    return bt_conn_start();
}

void bt_audio_disconnect(void)
{
    /* User-initiated drop: cancel any pending auto-retry. */
    s_conn_auto = false;
    if (s_conn_timer != NULL) {
        xTimerStop(s_conn_timer, 0);
    }
    if (s_connected) {
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
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
        if (!s_streaming || !s_enabled || s_tx_stopped) {
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

void bt_audio_enable(void)
{
    /* No Bluetooth in this build: nothing to bring up. */
}

void bt_audio_disable(void)
{
    /* No Bluetooth in this build: nothing to tear down. */
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

uint32_t bt_audio_device_version(void)
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

/* AVRCP handler registration. Lives outside the #if because it only stores the
 * callback pointers (defined above); the callbacks fire only when Bluetooth is
 * actually up, so no stub is needed for the no-BT build. */
void bt_audio_set_avrc_cmd_cb(bt_avrc_cmd_cb_t cb)
{
    s_avrc_cmd_cb = cb;
}

void bt_audio_set_avrc_volume_cb(bt_avrc_volume_cb_t cb)
{
    s_avrc_vol_cb = cb;
}

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
