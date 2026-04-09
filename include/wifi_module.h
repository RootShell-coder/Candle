#pragma once

#include <WiFi.h>

bool wifi_connect();
void wifi_handle_reconnect();
bool wifi_is_captive_portal_active();
