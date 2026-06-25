#pragma once

#include <WiFi.h>

struct WiFiCredentials {
  char ssid[64];
  char password[64];
};

bool wifi_connect();
void wifi_handle_reconnect();
bool wifi_is_captive_portal_active();
bool wifi_has_credentials();
bool wifi_get_credentials(WiFiCredentials& credentials);
bool wifi_save_credentials(const char* ssid, const char* password);
void wifi_clear_credentials();
