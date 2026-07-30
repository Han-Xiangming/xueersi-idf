/*
 * Hardware layer: lightweight Wi-Fi AP + Web provisioning.
 *
 * On first boot (no credentials stored in NVS) the device brings up an open
 * SoftAP "XIAOMAO_XXXX" and a tiny HTTP server (captive portal) on
 * 192.168.4.1. A phone joins the AP, opens the portal, scans nearby Wi-Fi
 * and submits SSID + password. The credentials are persisted in NVS and the
 * device switches to STA mode to join the user's network; the SoftAP and HTTP
 * server are torn down. Subsequent boots go straight to STA.
 *
 * Targets ESP-IDF v5.x. Every entry point is a safe no-op when Wi-Fi is
 * disabled in the build (CONFIG_ESP_WIFI_ENABLED). Coexists with the
 * BR/EDR-only Bluetooth stack: Wi-Fi is initialized before bt_audio_init()
 * and the controller's BR/EDR + Wi-Fi coexistence is managed by ESP-IDF.
 */
#pragma once

#include <stdbool.h>

/* Initialize NVS, the default netifs and the Wi-Fi subsystem (mode NULL).
 * Call once at startup, before bt_audio_init(). */
void wifi_prov_init(void);

/* Bring up provisioning: SoftAP + HTTP server when not yet configured, or
 * straight to STA connect when credentials exist. Call after all hardware
 * is up (so the LCD can show status). */
void wifi_prov_start(void);

/* Connect to the AP whose credentials are stored in NVS (STA mode). */
void wifi_prov_start_sta(void);

/* Tear down the SoftAP + HTTP server (used after a successful provisioning).
 * Safe to call when not in AP mode. */
void wifi_prov_stop(void);

/* True when credentials are stored in NVS (i.e. provisioning already done). */
bool wifi_prov_is_configured(void);

/* True when the STA is associated with an AP and has an IP. */
bool wifi_prov_is_connected(void);
