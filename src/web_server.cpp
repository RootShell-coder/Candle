#include <Arduino.h>
#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "config.h"
#include "i2c.h"
#include "metrics.h"
#include "ntp_module.h"
#include "sun_position.h"
#include "web_server.h"
#include "wifi_module.h"

static AsyncWebServer server(80);

namespace {
volatile bool s_restartScheduled = false;

template <size_t N>
// Копирует строковое поле JSON в фиксированный буфер конфигурации.
void copyJsonString(char (&dest)[N], JsonVariantConst value) {
  if (!value.is<const char*>()) {
    return;
  }

  const char* src = value.as<const char*>();
  if (src != nullptr) {
    strlcpy(dest, src, N);
  }
}

// Проверяет и извлекает яркость из JSON-поля. Основной формат — проценты 0..100,
// но старые значения 0..255 тоже принимаются и конвертируются.
bool parseJsonBrightness(JsonVariantConst value, uint8_t& brightness) {
  if (!(value.is<int>() || value.is<long>() || value.is<unsigned int>() || value.is<unsigned long>())) {
    return false;
  }

  long parsed = value.as<long>();
  if (parsed < 0 || parsed > 255) {
    return false;
  }

  if (parsed > 100) {
    parsed = (parsed * 100 + 127) / 255;
  }

  brightness = static_cast<uint8_t>(parsed);
  return true;
}

// Преобразует JSON-значение в булев флаг с поддержкой чисел.
bool parseJsonBool(JsonVariantConst value, bool& enabled) {
  if (value.is<bool>()) {
    enabled = value.as<bool>();
    return true;
  }

  if (value.is<int>() || value.is<long>() || value.is<unsigned int>() || value.is<unsigned long>()) {
    enabled = value.as<long>() != 0;
    return true;
  }

  return false;
}

// Извлекает координату из JSON, принимая и строку с точкой/запятой, и число.
bool parseJsonCoordinate(JsonVariantConst value, double& coordinate) {
  if (value.is<float>() || value.is<double>() ||
      value.is<int>() || value.is<long>() ||
      value.is<unsigned int>() || value.is<unsigned long>()) {
    coordinate = value.as<double>();
    return true;
  }

  if (!value.is<const char*>()) {
    return false;
  }

  String raw = value.as<const char*>();
  raw.trim();
  if (raw.isEmpty()) {
    return false;
  }

  raw.replace(',', '.');

  char* end = nullptr;
  const double parsed = strtod(raw.c_str(), &end);
  if (end == raw.c_str() || *end != '\0') {
    return false;
  }

  coordinate = parsed;
  return true;
}

// Читает булев параметр запроса из query или form-data.
bool parseBooleanValue(AsyncWebServerRequest* request, const char* name, bool& value) {
  const AsyncWebParameter* param = nullptr;

  if (request->hasParam(name)) {
    param = request->getParam(name);
  } else if (request->hasParam(name, true)) {
    param = request->getParam(name, true);
  }

  if (param == nullptr) {
    return false;
  }

  String raw = param->value();
  raw.trim();

  if (raw.equalsIgnoreCase("1") || raw.equalsIgnoreCase("true") || raw.equalsIgnoreCase("on")) {
    value = true;
    return true;
  }

  if (raw.equalsIgnoreCase("0") || raw.equalsIgnoreCase("false") || raw.equalsIgnoreCase("off")) {
    value = false;
    return true;
  }

  return false;
}

// Перезапускает ESP32 после небольшой задержки в отдельной задаче.
void restartDeviceTask(void* param) {
  const uint32_t delayMs = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(param));
  vTaskDelay(pdMS_TO_TICKS(delayMs));
  Serial.println("[web] restarting device after settings change");
  ESP.restart();
}

// Планирует отложенный рестарт после сохранения критичных настроек.
void scheduleDeviceRestart(uint32_t delayMs) {
  if (s_restartScheduled) {
    return;
  }

  s_restartScheduled = true;
  if (xTaskCreate(restartDeviceTask,
                  "web_reset_restart",
                  2048,
                  reinterpret_cast<void *>(static_cast<uintptr_t>(delayMs)),
                  1,
                  nullptr) != pdPASS) {
    s_restartScheduled = false;
    Serial.println("[web] failed to schedule restart task, restarting immediately");
    delay(delayMs);
    ESP.restart();
  }
}
}  // namespace

// Собирает JSON со всеми настройками для web-интерфейса.
static String buildSettingsPayload() {
  const Config& cfg = getConfig();

  JsonDocument doc;
  doc["devname"] = cfg.wifi.devname;
  doc["name"] = cfg.wifi.name;
  doc["ssid"] = cfg.wifi.ssid;
  doc["password"] = cfg.wifi.password;
  doc["ntp_server"] = cfg.ntp.ntp_server;
  doc["ntp_timezone"] = cfg.ntp.ntp_timezone;
  doc["lat"] = cfg.location.lat;
  doc["lon"] = cfg.location.lng;
  doc["autoMode"] = cfg.location.enabled;
  doc["sunMode"] = sun_position_current_mode_name();
  doc["autoCandleOn"] = sun_position_should_enable_now();
  doc["brightness"] = cfg.brightness;
  doc["candleOn"] = cfg.candleOn;

  String payload;
  serializeJson(doc, payload);
  return payload;
}

// Формирует компактный JSON с датой и текущим солнечным режимом.
static String buildDatePayload() {
  JsonDocument doc;
  char formatted[24] = "--:-- --.--.----";
  bool valid = false;

  if (ntp_has_valid_time()) {
    const time_t now = time(nullptr);
    struct tm localTm {};

    if (localtime_r(&now, &localTm) != nullptr &&
        strftime(formatted, sizeof(formatted), "%H:%M %d.%m.%Y", &localTm) > 0) {
      valid = true;
    }
  }

  doc["valid"] = valid;
  doc["date"] = formatted;
  doc["sunMode"] = sun_position_current_mode_name();
  doc["autoCandleOn"] = sun_position_should_enable_now();
  doc["candleOn"] = sun_position_is_candle_enabled();

  String payload;
  serializeJson(doc, payload);
  return payload;
}

// Извлекает значение яркости из HTTP-параметров. Основной диапазон — 0..100%,
// старые запросы 0..255 автоматически конвертируются.
static bool parseBrightnessValue(AsyncWebServerRequest* request, uint8_t& value) {
  const AsyncWebParameter* param = nullptr;

  if (request->hasParam("value")) {
    param = request->getParam("value");
  } else if (request->hasParam("brightness")) {
    param = request->getParam("brightness");
  } else if (request->hasParam("value", true)) {
    param = request->getParam("value", true);
  } else if (request->hasParam("brightness", true)) {
    param = request->getParam("brightness", true);
  }

  if (param == nullptr) {
    return false;
  }

  const String raw = param->value();
  if (raw.isEmpty()) {
    return false;
  }

  char *end = nullptr;
  long parsed = strtol(raw.c_str(), &end, 10);
  if (end == raw.c_str() || *end != '\0' || parsed < 0 || parsed > 255) {
    return false;
  }

  if (parsed > 100) {
    parsed = (parsed * 100 + 127) / 255;
  }

  value = static_cast<uint8_t>(parsed);
  return true;
}

// Поднимает HTTP API и статическую раздачу файлов после готовности сети.
void web_server_setup_with_wifi() {
  if (WiFi.status() != WL_CONNECTED) {
    if (!wifi_connect()) {
      Serial.println("[web] WiFi not connected, web server disabled");
      return;
    }
  }

  // Обработка / и index.html
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    metrics_record_http_request();
    if (LittleFS.exists("/index.html")) {
      request->send(LittleFS, "/index.html", "text/html");
    } else {
      request->send(200, "text/plain", "index.html not found");
    }
  });

  server.serveStatic("/index.html", LittleFS, "/index.html").setDefaultFile("index.html");

  // API
  server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest *request){
    metrics_record_http_request();
    request->send(200, "application/json", buildSettingsPayload());
  });

  server.on("/api/date", HTTP_GET, [](AsyncWebServerRequest *request){
    metrics_record_http_request();
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", buildDatePayload());
    response->addHeader("Cache-Control", "no-store, max-age=0");
    request->send(response);
  });

  server.on("/api/save", HTTP_POST, [](AsyncWebServerRequest *request) {
    (void)request;
  }, nullptr, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    if (index == 0) {
      metrics_record_http_request();
      String* body = new String();
      body->reserve(total);
      request->_tempObject = body;
    }

    String* body = static_cast<String*>(request->_tempObject);
    if (body == nullptr) {
      request->send(500, "application/json", "{\"status\":\"error\",\"message\":\"request buffer allocation failed\"}");
      return;
    }

    body->concat(reinterpret_cast<const char*>(data), len);
    if (index + len != total) {
      return;
    }

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, *body);
    delete body;
    request->_tempObject = nullptr;

    if (error) {
      request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"invalid JSON payload\"}");
      return;
    }

    Config& cfg = getMutableConfig();
    const JsonObject root = doc.as<JsonObject>();

    copyJsonString(cfg.wifi.devname, root["devname"]);
    copyJsonString(cfg.wifi.name, root["name"]);
    copyJsonString(cfg.wifi.ssid, root["ssid"]);
    copyJsonString(cfg.wifi.password, root["password"]);
    copyJsonString(cfg.ntp.ntp_server, root["ntp_server"]);
    copyJsonString(cfg.ntp.ntp_timezone, root["ntp_timezone"]);

    double latitude = cfg.location.lat;
    if (parseJsonCoordinate(root["lat"], latitude) && latitude >= -90.0 && latitude <= 90.0) {
      cfg.location.lat = latitude;
    }

    double longitude = cfg.location.lng;
    if (parseJsonCoordinate(root["lon"], longitude) && longitude >= -180.0 && longitude <= 180.0) {
      cfg.location.lng = longitude;
    }

    bool autoMode = cfg.location.enabled;
    if (parseJsonBool(root["autoMode"], autoMode)) {
      cfg.location.enabled = autoMode;
    }

    uint8_t brightness = cfg.brightness;
    if (parseJsonBrightness(root["brightness"], brightness)) {
      cfg.brightness = brightness;
    }

    if (!cfg.location.enabled) {
      bool candleOn = cfg.candleOn;
      if (parseJsonBool(root["candleOn"], candleOn)) {
        cfg.candleOn = candleOn;
      }
      if (cfg.brightness == 0) {
        cfg.candleOn = false;
      }
    }

    if (!configSave()) {
      request->send(500, "application/json", "{\"status\":\"error\",\"message\":\"failed to save settings\"}");
      return;
    }

    const bool outputOn = cfg.location.enabled ? sun_position_should_enable_now() : cfg.candleOn;
    i2c_set_brightness(outputOn ? cfg.brightness : 0);
    request->send(200, "application/json", "{\"status\":\"ok\",\"restarting\":true,\"message\":\"settings saved, device restarting\"}");
    scheduleDeviceRestart(750);
  });

  server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest *request){
    metrics_record_http_request();
    configResetToDefault();

    if (!configSave()) {
      request->send(500, "application/json", "{\"status\":\"error\",\"message\":\"failed to clear Wi-Fi credentials\"}");
      return;
    }

    i2c_set_brightness(getConfig().brightness);
    request->send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Wi-Fi credentials cleared, device restarting\"}");
    scheduleDeviceRestart(750);
  });

  server.on("/api/brightness", HTTP_ANY, [](AsyncWebServerRequest *request){
    metrics_record_http_request();

    if (request->method() == HTTP_GET) {
      String payload = "{\"status\":\"ok\",\"brightness\":" + String(getConfig().brightness) + "}";
      request->send(200, "application/json", payload);
      return;
    }

    uint8_t value = 0;
    if (!parseBrightnessValue(request, value)) {
      request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"brightness must be an integer percent from 0 to 100\"}");
      return;
    }

    Config& cfg = getMutableConfig();

    bool autoMode = cfg.location.enabled;
    if (parseBooleanValue(request, "autoMode", autoMode)) {
      cfg.location.enabled = autoMode;
    }

    cfg.brightness = value;

    if (!cfg.location.enabled) {
      bool candleOn = cfg.candleOn;
      if (parseBooleanValue(request, "candleOn", candleOn)) {
        cfg.candleOn = candleOn;
      }
      if (value == 0) {
        cfg.candleOn = false;
      }
    }

    const bool autoCandleOn = sun_position_should_enable_now();
    const bool outputOn = cfg.location.enabled ? autoCandleOn : cfg.candleOn;
    i2c_set_brightness(outputOn ? value : 0);

    if (!configSave()) {
      request->send(500, "application/json", "{\"status\":\"error\",\"message\":\"failed to save brightness\"}");
      return;
    }

    String payload = String("{\"status\":\"ok\",\"brightness\":") +
                     String(value) +
                     ",\"candleOn\":" +
                     (cfg.candleOn ? "true" : "false") +
                     ",\"autoMode\":" +
                     (cfg.location.enabled ? "true" : "false") +
                     ",\"autoCandleOn\":" +
                     (autoCandleOn ? "true" : "false") +
                     ",\"sunMode\":\"" +
                     String(sun_position_current_mode_name()) +
                     "\"}";
    request->send(200, "application/json", payload);
  });

  server.on("/metrics", HTTP_GET, [](AsyncWebServerRequest *request){
    metrics_record_http_request();
    metrics_record_prometheus_scrape();
    request->send(200, "text/plain; version=0.0.4; charset=utf-8", metrics_render_prometheus());
  });

  server.onNotFound([](AsyncWebServerRequest *request){
    metrics_record_http_request();

    if (wifi_is_captive_portal_active()) {
      request->redirect(String("http://") + WiFi.softAPIP().toString() + "/");
      return;
    }

    request->send(404, "text/plain", "Not found");
  });

  // Обработка статики (например, /style.css)
  server.serveStatic("/", LittleFS, "/");

  server.begin();
  Serial.println("[web] server started");
}

// Хук оставлен для совместимости, так как сервер работает асинхронно.
void web_server_handle() {
}
