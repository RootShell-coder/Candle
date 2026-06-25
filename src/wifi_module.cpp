#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_netif.h>

#include "config.h"
#include "metrics.h"
#include "wifi_module.h"

namespace {
constexpr uint32_t kWifiRetryIntervalMs = 10000;
constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr uint16_t kCaptivePortalDnsPort = 53;
constexpr const char* kSetupApSsid = "candle-setup";
constexpr const char* kSetupApPassword = "candle1234";
constexpr const char* kPrefsNamespace = "wifi";
constexpr const char* kPrefsSsidKey = "ssid";
constexpr const char* kPrefsPasswordKey = "password";

const IPAddress kCaptivePortalIp(192, 168, 4, 1);
const IPAddress kCaptivePortalGateway(192, 168, 4, 1);
const IPAddress kCaptivePortalSubnet(255, 255, 255, 0);

DNSServer s_dnsServer;
WiFiCredentials s_credentials {};
uint32_t s_lastConnectAttemptMs = 0;
bool s_connectInProgress = false;
bool s_connectionLogged = false;
bool s_wasConnected = false;
bool s_captivePortalActive = false;

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

const char* wifi_hostname() {
  static char hostname[32] = "candle-light";
  const Config cfg = getConfig();
  strlcpy(hostname, cfg.devname[0] != '\0' ? cfg.devname : "candle-light", sizeof(hostname));
  return hostname;
}

bool load_credentials_from_nvs(WiFiCredentials& credentials) {
  memset(&credentials, 0, sizeof(credentials));

  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    Serial.println("[wifi] failed to open Wi-Fi preferences for reading");
    return false;
  }

  const String ssid = prefs.getString(kPrefsSsidKey, "");
  const String password = prefs.getString(kPrefsPasswordKey, "");
  prefs.end();

  ssid.toCharArray(credentials.ssid, sizeof(credentials.ssid));
  password.toCharArray(credentials.password, sizeof(credentials.password));
  return credentials.ssid[0] != '\0';
}

void wifi_stop_setup_ap() {
  if (!s_captivePortalActive) {
    return;
  }

  s_dnsServer.stop();
  WiFi.softAPdisconnect(true);
  s_captivePortalActive = false;
  Serial.println("[wifi] setup AP stopped");
}

bool wifi_start_setup_ap() {
  if (s_captivePortalActive) {
    s_dnsServer.processNextRequest();
    return true;
  }

  WiFi.mode(WIFI_AP_STA);
  wifi_apply_hostname(wifi_hostname(), true, true);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  if (!WiFi.softAPConfig(kCaptivePortalIp, kCaptivePortalGateway, kCaptivePortalSubnet)) {
    Serial.println("[wifi] setup AP IP config failed");
  }

  if (!WiFi.softAP(kSetupApSsid, kSetupApPassword)) {
    Serial.printf("[wifi] failed to start setup AP '%s'\n", kSetupApSsid);
    return false;
  }

  s_dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  s_dnsServer.start(kCaptivePortalDnsPort, "*", WiFi.softAPIP());
  s_captivePortalActive = true;

  Serial.printf(
      "[wifi] setup AP '%s' started at %s\n",
      kSetupApSsid,
      WiFi.softAPIP().toString().c_str());

  return true;
}

void wifi_begin_connection(const WiFiCredentials& credentials) {
  WiFi.mode(s_captivePortalActive ? WIFI_AP_STA : WIFI_STA);
  wifi_apply_hostname(wifi_hostname(), true, s_captivePortalActive);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(credentials.ssid, credentials.password);

  metrics_record_wifi_connect_attempt();
  s_lastConnectAttemptMs = millis();
  s_connectInProgress = true;
  s_connectionLogged = false;

  Serial.printf("[wifi] connecting to STA '%s'...\n", credentials.ssid);
}
}  // namespace

void wifi_handle_reconnect() {
  if (s_captivePortalActive) {
    s_dnsServer.processNextRequest();
  }

  (void)wifi_connect();
}

bool wifi_is_captive_portal_active() {
  return s_captivePortalActive;
}

bool wifi_has_credentials() {
  WiFiCredentials credentials {};
  return load_credentials_from_nvs(credentials);
}

bool wifi_get_credentials(WiFiCredentials& credentials) {
  return load_credentials_from_nvs(credentials);
}

bool wifi_save_credentials(const char* ssid, const char* password) {
  if (ssid == nullptr || ssid[0] == '\0') {
    wifi_clear_credentials();
    return true;
  }

  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    Serial.println("[wifi] failed to open Wi-Fi preferences for writing");
    return false;
  }

  const size_t ssidWritten = prefs.putString(kPrefsSsidKey, ssid);
  const size_t passwordWritten = prefs.putString(kPrefsPasswordKey, password != nullptr ? password : "");
  prefs.end();

  (void)passwordWritten;
  const bool ok = ssidWritten > 0;
  if (ok) {
    strlcpy(s_credentials.ssid, ssid, sizeof(s_credentials.ssid));
    strlcpy(s_credentials.password, password != nullptr ? password : "", sizeof(s_credentials.password));
    s_connectInProgress = false;
    s_connectionLogged = false;
  }

  return ok;
}

void wifi_clear_credentials() {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.remove(kPrefsSsidKey);
    prefs.remove(kPrefsPasswordKey);
    prefs.end();
  }

  memset(&s_credentials, 0, sizeof(s_credentials));
  WiFi.disconnect(false, false);
  s_connectInProgress = false;
  s_connectionLogged = false;
  s_wasConnected = false;
}

bool wifi_connect() {
  if (!load_credentials_from_nvs(s_credentials)) {
    wifi_start_setup_ap();
    return false;
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

    if (s_captivePortalActive) {
      wifi_stop_setup_ap();
      WiFi.mode(WIFI_STA);
      wifi_apply_hostname(wifi_hostname(), true, false);
    }

    s_wasConnected = true;
    s_connectInProgress = false;
    return true;
  }

  const uint32_t now = millis();
  if (!s_connectInProgress) {
    wifi_begin_connection(s_credentials);
    return false;
  }

  if ((now - s_lastConnectAttemptMs) >= kWifiConnectTimeoutMs) {
    Serial.println("[wifi] STA connect timeout, setup AP will stay available while retrying");
    wifi_start_setup_ap();
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
