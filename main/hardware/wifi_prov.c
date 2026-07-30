/*
 * Hardware layer: lightweight Wi-Fi AP + Web provisioning.
 * See hardware/wifi_prov.h.
 *
 * Two boot paths:
 *   - no creds in NVS  -> SoftAP "XIAOMAO_XXXX" + HTTP captive portal on
 *                          192.168.4.1, phone scans + submits SSID/pass,
 *                          creds saved to NVS, AP/HTTP torn down, STA brought
 *                          up against the just-saved creds.
 *   - creds in NVS      -> straight to STA connect, no SoftAP at all.
 *
 * The HTTP server is intentionally small and self-contained: an embedded
 * HTML form (string constant), a /scan JSON endpoint that wraps
 * esp_wifi_scan_start, and a /connect POST handler that parses a tiny
 * urlencoded body. No protobuf, no wifi_provisioning component, no
 * filesystem. Captive-portal probes from Android (/generate_204) and Apple
 * (/hotspot-detect.html) are served so the OS pops the portal page.
 */
#include "hardware/wifi_prov.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "wifi_prov";

#if defined(CONFIG_ESP_WIFI_ENABLED)

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_mac.h"

/* NVS layout: namespace "prov", two string keys. */
#define PROV_NVS_NS         "prov"
#define PROV_KEY_SSID       "ssid"
#define PROV_KEY_PASS       "pass"

#define PROV_MAX_SSID_LEN   32      /* matches wifi_config_t.sta.ssid[32] */
#define PROV_MAX_PASS_LEN   64      /* wifi_config_t.sta.password[64] */
#define PROV_AP_MAX_CONN    4
#define PROV_AP_CHANNEL      6
#define PROV_SCAN_MAX       16      /* APs returned by /scan */
#define PROV_HTTP_STACK     6144    /* /scan buffers 16 APs + 2KB JSON */
#define PROV_SWITCH_DELAY_MS 800    /* let the /connect response flush first */

static volatile bool s_initialized;  /* wifi_prov_init() has run */
static volatile bool s_configured;   /* credentials exist in NVS */
static volatile bool s_connected;    /* STA got an IP */
static volatile bool s_ap_mode;      /* SoftAP + HTTP are up */
static volatile int  s_retry;       /* STA disconnect retries */

static httpd_handle_t s_server;
static esp_netif_t   *s_ap_netif;
static esp_netif_t   *s_sta_netif;

/* ---- NVS helpers ------------------------------------------------------- */

/* Read the saved credentials. Returns false when missing / unreadable. */
static bool load_creds(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap)
{
    nvs_handle_t h;
    if (nvs_open(PROV_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t sl = ssid_cap, pl = pass_cap;
    esp_err_t e1 = nvs_get_str(h, PROV_KEY_SSID, ssid, &sl);
    esp_err_t e2 = nvs_get_str(h, PROV_KEY_PASS, pass, &pl);
    nvs_close(h);
    return e1 == ESP_OK && sl > 0 && sl <= ssid_cap && e2 == ESP_OK;
}

/* Persist the given credentials (pass may be empty for open networks). */
static void save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(PROV_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, PROV_KEY_SSID, ssid);
    nvs_set_str(h, PROV_KEY_PASS, pass ? pass : "");
    nvs_commit(h);
    nvs_close(h);
}

/* Copy a NUL-terminated string into a fixed-size field, clamping to fit.
 * memcpy (unlike strncpy/snprintf) carries no NUL-termination semantics, so
 * it sidesteps both -Wstringop-truncation and -Wformat-truncation. */
static void copy_field(char *dst, const char *src, size_t dst_size)
{
    size_t len = strlen(src);
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* Build a STA wifi_config_t from plain strings. */
static void fill_sta_config(wifi_config_t *cfg, const char *ssid, const char *pass)
{
    memset(cfg, 0, sizeof(*cfg));
    copy_field((char *)cfg->sta.ssid,     ssid, sizeof(cfg->sta.ssid));
    copy_field((char *)cfg->sta.password, pass, sizeof(cfg->sta.password));
}

bool wifi_prov_is_configured(void) { return s_configured; }
bool wifi_prov_is_connected(void)  { return s_connected; }

/* ---- Wi-Fi event handlers ---------------------------------------------- */

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retry < 5) {
            s_retry++;
            esp_wifi_connect();
            ESP_LOGW(TAG, "STA disconnect, retry %d", s_retry);
        } else {
            ESP_LOGE(TAG, "STA connect failed after retries");
        }
    } else if (id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "phone joined the SoftAP");
    }
}

static void on_ip_event(void *arg, esp_event_base_t base,
                        int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        s_retry = 0;
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "STA got IP " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

/* ---- Embedded HTML ----------------------------------------------------- */

static const char *PAGE_HTML =
"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>Xiaomao WiFi</title>"
"<style>body{font-family:system-ui,sans-serif;max-width:360px;margin:20px auto}"
"input,select,button{display:block;width:100%;padding:9px;margin:6px 0;box-sizing:border-box}"
"button{background:#2563eb;color:#fff;border:none;border-radius:5px}"
"</style></head><body>"
"<h2>Xiaomao WiFi Setup</h2>"
"<label>Networks:</label>"
"<select id=\"s\"></select>"
"<button onclick=\"scan()\">Rescan</button>"
"<label>SSID:</label>"
"<input id=\"t\" placeholder=\"SSID\">"
"<label>Password:</label>"
"<input id=\"p\" type=\"password\" placeholder=\"Password\">"
"<button onclick=\"go()\">Connect</button>"
"<p id=\"r\"></p>"
"<script>"
"function scan(){fetch('/scan').then(r=>r.json()).then(a=>{"
"var s=document.getElementById('s');s.innerHTML='';"
"a.forEach(x=>{var o=document.createElement('option');o.value=x.s;"
"o.text=x.s+' ('+x.r+')';s.appendChild(o)});"
"if(a.length)document.getElementById('t').value=a[0].s})}"
"function go(){var s=document.getElementById('t').value;"
"var p=document.getElementById('p').value;"
"document.getElementById('r').textContent='Connecting...';"
"fetch('/connect',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)})"
".then(r=>r.text()).then(t=>{document.getElementById('r').textContent=t})}"
"scan();"
"</script></body></html>";

static const char *PAGE_OK =
"<!DOCTYPE html><html><body style=\"font-family:sans-serif;text-align:center;"
"margin-top:40px\"><h2>Saved!</h2>"
"<p>The device is now connecting to your Wi-Fi.<br>"
"You may close this page; the hotspot will shut down shortly.</p></body></html>";

/* ---- HTTP handlers ----------------------------------------------------- */

/* GET / : the portal page (also used for captive-portal probes). */
static esp_err_t handler_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

/* GET /scan : blocking active scan, JSON array of nearby APs.
 * SSID is JSON-escaped (quotes, backslash, control chars) so the response
 * stays valid even with exotic network names. */
static esp_err_t handler_scan(httpd_req_t *req)
{
    wifi_scan_config_t sc = {
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "[]", 2);
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > PROV_SCAN_MAX) {
        n = PROV_SCAN_MAX;
    }
    wifi_ap_record_t aps[PROV_SCAN_MAX];
    if (n > 0) {
        esp_wifi_scan_get_ap_records(&n, aps);
    }

    /* Cap entries when the buffer is nearly full (worst case ~50B/AP). */
    char buf[2048];
    size_t off = 0;
    off += sprintf(buf + off, "[");
    for (int i = 0; i < n && off < 1900; i++) {
        off += sprintf(buf + off, "%s{\"s\":\"", i ? "," : "");
        for (int j = 0; aps[i].ssid[j] && off < 1950; j++) {
            char c = aps[i].ssid[j];
            if (c == '"' || c == '\\') {
                buf[off++] = '\\';
                buf[off++] = c;
            } else if ((unsigned char)c < 0x20) {
                off += sprintf(buf + off, "\\u%04x", (unsigned char)c);
            } else {
                buf[off++] = c;
            }
        }
        off += sprintf(buf + off, "\",\"r\":%d}", aps[i].rssi);
    }
    off += sprintf(buf + off, "]");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, off);
}

/* URL-decode a small form field in place. Returns the decoded length. */
static int url_decode(char *dst, const char *src, int max)
{
    int j = 0;
    for (int i = 0; src[i] && j < max - 1; i++) {
        char c = src[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && src[i + 1] && src[i + 2]) {
            char hex[3] = { src[i + 1], src[i + 2], 0 };
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 2;
            continue;
        }
        dst[j++] = c;
    }
    dst[j] = 0;
    return j;
}

/* ---- Background: switch AP -> STA after provisioning ------------------- */

/* Tear down AP + HTTP, then bring up STA with the just-saved creds. Runs in
 * its own task so the /connect HTTP response can finish flushing first. */
static void prov_switch_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(PROV_SWITCH_DELAY_MS));

    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    /* Switching mode from APSTA to STA keeps the radio up; the AP vanishes
     * and the STA side gets the full airtime. */
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_ap_mode = false;
    ESP_LOGI(TAG, "SoftAP + HTTP torn down, STA mode");

    /* Configure STA with the just-saved creds and connect. The radio is
     * already started, so we only need set_config + connect. */
    char ssid[PROV_MAX_SSID_LEN + 1];
    char pass[PROV_MAX_PASS_LEN + 1];
    if (load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        wifi_config_t cfg;
        fill_sta_config(&cfg, ssid, pass);
        esp_wifi_set_config(WIFI_IF_STA, &cfg);
        esp_wifi_connect();
        ESP_LOGI(TAG, "STA connecting to \"%s\"", ssid);
    }

    vTaskDelete(NULL);
}

/* POST /connect : body "ssid=...&pass=..." -> save creds, reply OK, then
 * switch to STA in the background. */
static esp_err_t handler_connect(httpd_req_t *req)
{
    char body[512];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_OK;
    }
    body[len] = 0;

    char ssid[PROV_MAX_SSID_LEN + 1] = {0};
    char pass[PROV_MAX_PASS_LEN + 1] = {0};
    char *tok = strtok(body, "&");
    while (tok) {
        if (strncmp(tok, "ssid=", 5) == 0) {
            url_decode(ssid, tok + 5, sizeof(ssid));
        } else if (strncmp(tok, "pass=", 5) == 0) {
            url_decode(pass, tok + 5, sizeof(pass));
        }
        tok = strtok(NULL, "&");
    }

    if (ssid[0] == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "got credentials for \"%s\"", ssid);
    save_creds(ssid, pass);
    s_configured = true;

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, PAGE_OK, HTTPD_RESP_USE_STRLEN);

    /* Tear down AP + bring up STA in the background, after the HTTP
     * response has had time to flush to the phone. */
    xTaskCreate(prov_switch_task, "prov_sw", PROV_HTTP_STACK, NULL, 5, NULL);
    return ESP_OK;
}

static void start_http(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size       = PROV_HTTP_STACK;
    cfg.max_uri_handlers = 8;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start HTTP server");
        return;
    }

    static const httpd_uri_t u_root   = { .uri = "/",                    .method = HTTP_GET,  .handler = handler_root };
    static const httpd_uri_t u_scan   = { .uri = "/scan",                .method = HTTP_GET,  .handler = handler_scan };
    static const httpd_uri_t u_conn   = { .uri = "/connect",             .method = HTTP_POST, .handler = handler_connect };
    static const httpd_uri_t u_gen204 = { .uri = "/generate_204",        .method = HTTP_GET,  .handler = handler_root };
    static const httpd_uri_t u_hotsp  = { .uri = "/hotspot-detect.html", .method = HTTP_GET,  .handler = handler_root };

    httpd_register_uri_handler(s_server, &u_root);
    httpd_register_uri_handler(s_server, &u_scan);
    httpd_register_uri_handler(s_server, &u_conn);
    httpd_register_uri_handler(s_server, &u_gen204);
    httpd_register_uri_handler(s_server, &u_hotsp);
}

/* ---- SoftAP bring-up --------------------------------------------------- */

static void start_ap(void)
{
    /* AP SSID = XIAOMAO_ + last 2 MAC bytes (upper case hex). */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[20];
    snprintf(ssid, sizeof(ssid), "XIAOMAO_%02X%02X", mac[4], mac[5]);

    wifi_config_t cfg = {0};
    copy_field((char *)cfg.ap.ssid, ssid, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len      = (uint8_t)strlen(ssid);
    cfg.ap.channel       = PROV_AP_CHANNEL;
    cfg.ap.max_connection = PROV_AP_MAX_CONN;
    cfg.ap.authmode      = WIFI_AUTH_OPEN;

    /* APSTA so /scan can run while the AP is up. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_ap_mode = true;
    start_http();
    ESP_LOGI(TAG, "SoftAP \"%s\" up, open http://192.168.4.1", ssid);
}

/* ---- Public entry points ----------------------------------------------- */

void wifi_prov_init(void)
{
    if (s_initialized) {
        return;
    }

    /* NVS (also required by Wi-Fi for calibration storage). Erase + retry
     * on the legacy / out-of-space cases so an OTA reflash just works. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    esp_netif_init();
    esp_event_loop_create_default();
    s_ap_netif  = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));

    /* Probe NVS so we know which boot path to take. */
    char ssid[PROV_MAX_SSID_LEN + 1];
    char pass[PROV_MAX_PASS_LEN + 1];
    s_configured = load_creds(ssid, sizeof(ssid), pass, sizeof(pass));

    esp_event_handler_instance_t wi, ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        on_wifi_event, NULL, &wi);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        on_ip_event, NULL, &ip);
    (void)wi; (void)ip;   /* kept for the lifetime of the program */

    s_initialized = true;
    ESP_LOGI(TAG, "init done (configured=%d)", s_configured);
}

void wifi_prov_start_sta(void)
{
    char ssid[PROV_MAX_SSID_LEN + 1];
    char pass[PROV_MAX_PASS_LEN + 1];
    if (!load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGE(TAG, "start_sta: no credentials");
        return;
    }

    wifi_config_t cfg;
    fill_sta_config(&cfg, ssid, pass);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();
    ESP_LOGI(TAG, "STA connecting to \"%s\"", ssid);
}

void wifi_prov_start(void)
{
    if (!s_initialized) {
        wifi_prov_init();
    }
    if (s_configured) {
        wifi_prov_start_sta();
    } else {
        start_ap();
    }
}

void wifi_prov_stop(void)
{
    if (!s_ap_mode) {
        return;
    }
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_ap_mode = false;
    ESP_LOGI(TAG, "SoftAP + HTTP torn down (external stop)");
}

#else  /* !CONFIG_ESP_WIFI_ENABLED */

void wifi_prov_init(void)        {}
void wifi_prov_start(void)      {}
void wifi_prov_start_sta(void)  {}
void wifi_prov_stop(void)       {}
bool wifi_prov_is_configured(void) { return false; }
bool wifi_prov_is_connected(void)   { return false; }

#endif
