#include <Arduino.h>
#include <DNSServer.h>
#include <WiFi.h>
#include <esp_netif.h>

#include "config.h"
#include "metrics.h"
#include "wifi_module.h"

namespace {
constexpr uint32_t kWifiRetryIntervalMs = 10000;
constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr uint16_t kCaptivePortalDnsPort = 53;
const IPAddress kCaptivePortalIp(192, 168, 4, 1);
const IPAddress kCaptivePortalGateway(192, 168, 4, 1);
const IPAddress kCaptivePortalSubnet(255, 255, 255, 0);

DNSServer s_dnsServer;
uint32_t s_lastConnectAttemptMs = 0;
bool s_connectInProgress = false;
bool s_connectionLogged = false;
bool s_wasConnected = false;
bool s_captivePortalActive = false;

// Возвращает hostname устройства или безопасное имя по умолчанию.
const char* wifi_hostname(const Config& cfg) {
  return strlen(cfg.wifi.devname) != 0 ? cfg.wifi.devname : "candle_light";
}

// Применяет hostname одновременно через Arduino API и напрямую в netif.
void wifi_apply_hostname(const char* hostname, bool applySta, bool applyAp) {
  if (hostname == nullptr || hostname[0] == '\0') {
    return;
  }

  WiFi.setHostname(hostname);

  if (applySta) {
    if (esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
      esp_netif_set_hostname(sta, hostname);
    }
  }

  if (applyAp) {
    if (esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF")) {
      esp_netif_set_hostname(ap, hostname);
    }
  }
}

// Останавливает captive portal, если он сейчас активен.
void wifi_stop_captive_portal() {
  if (!s_captivePortalActive) {
    return;
  }

  s_dnsServer.stop();
  WiFi.softAPdisconnect(true);
  s_captivePortalActive = false;
  Serial.println("[wifi] captive portal stopped");
}

// Запускает точку доступа и DNS-перенаправление для первичной настройки Wi‑Fi.
bool wifi_start_captive_portal(const Config& cfg) {
  if (s_captivePortalActive) {
    s_dnsServer.processNextRequest();
    return true;
  }

  const char* apName = wifi_hostname(cfg);

  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP_STA);
  wifi_apply_hostname(apName, true, true);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  if (!WiFi.softAPConfig(kCaptivePortalIp, kCaptivePortalGateway, kCaptivePortalSubnet)) {
    Serial.println("[wifi] captive portal IP config failed");
  }

  if (!WiFi.softAP(apName)) {
    Serial.printf("[wifi] failed to start captive portal '%s'\n", apName);
    return false;
  }

  s_dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  s_dnsServer.start(kCaptivePortalDnsPort, "*", WiFi.softAPIP());

  s_lastConnectAttemptMs = millis();
  s_connectInProgress = false;
  s_connectionLogged = false;
  s_wasConnected = false;
  s_captivePortalActive = true;

  Serial.printf(
      "[wifi] SSID/password empty, captive portal '%s' started at %s\n",
      apName,
      WiFi.softAPIP().toString().c_str());

  return true;
}

// Начинает новое подключение к сохранённой Wi‑Fi-сети.
void wifi_begin_connection(const Config& cfg) {
  wifi_stop_captive_portal();

  WiFi.mode(WIFI_STA);
  wifi_apply_hostname(wifi_hostname(cfg), true, false);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(cfg.wifi.ssid, cfg.wifi.password);

  metrics_record_wifi_connect_attempt();
  s_lastConnectAttemptMs = millis();
  s_connectInProgress = true;
  s_connectionLogged = false;

  Serial.printf("[wifi] connecting to '%s'...\n", cfg.wifi.ssid);
}
}  // namespace

// Обрабатывает DNS captive portal и периодический reconnect.
void wifi_handle_reconnect() {
  if (s_captivePortalActive) {
    s_dnsServer.processNextRequest();
  }

  (void)wifi_connect();
}

// Возвращает флаг активности captive portal.
bool wifi_is_captive_portal_active() {
  return s_captivePortalActive;
}

// Поддерживает актуальное состояние Wi‑Fi: портал, подключение и повторные попытки.
bool wifi_connect() {
  const Config& cfg = getConfig();

  if (strlen(cfg.wifi.ssid) == 0) {
    return wifi_start_captive_portal(cfg);
  }

  if (s_captivePortalActive) {
    wifi_stop_captive_portal();
  }

  const wl_status_t status = WiFi.status();
  if (s_wasConnected && status != WL_CONNECTED) {
    metrics_record_wifi_disconnect();
    s_wasConnected = false;
    s_connectionLogged = false;
  }

  if (status == WL_CONNECTED) {
    if (!s_connectionLogged) {
      Serial.print("[wifi] connected, IP: ");
      Serial.println(WiFi.localIP());
      metrics_record_wifi_connected();
      s_connectionLogged = true;
    }
    s_wasConnected = true;
    s_connectInProgress = false;
    return true;
  }

  const uint32_t now = millis();
  if (!s_connectInProgress) {
    wifi_begin_connection(cfg);
    return false;
  }

  if ((now - s_lastConnectAttemptMs) >= kWifiConnectTimeoutMs) {
    Serial.println("[wifi] connect timeout, will retry without reboot");
    WiFi.disconnect(false, false);
    s_connectInProgress = false;
    s_lastConnectAttemptMs = now;
    return false;
  }

  if ((now - s_lastConnectAttemptMs) >= kWifiRetryIntervalMs && status != WL_IDLE_STATUS) {
    s_connectInProgress = false;
  }

  return false;
}
