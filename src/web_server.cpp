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
#include "moon_phase.h"
#include "ntp_module.h"
#include "sun_offline.h"
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

// Извлекает координату из query/form-параметра; поддерживает десятичную точку и запятую.
bool parseCoordinateParameter(AsyncWebServerRequest* request, const char* name, double& value) {
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
  if (raw.isEmpty()) {
    return false;
  }

  raw.replace(',', '.');

  char* end = nullptr;
  const double parsed = strtod(raw.c_str(), &end);
  if (end == raw.c_str() || *end != '\0') {
    return false;
  }

  value = parsed;
  return true;
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

static void sendJsonDocument(AsyncWebServerRequest* request, int code, JsonDocument& doc, bool noStore = false) {
  AsyncResponseStream* response = request->beginResponseStream("application/json");
  response->setCode(code);
  if (noStore) {
    response->addHeader("Cache-Control", "no-store, max-age=0");
  }
  serializeJson(doc, *response);
  request->send(response);
}

static void sendStatusMessage(AsyncWebServerRequest* request, int code, const char* status, const char* message) {
  JsonDocument doc;
  doc["status"] = status;
  doc["message"] = message;
  sendJsonDocument(request, code, doc);
}

// Собирает JSON со всеми настройками для web-интерфейса.
static void populateSettingsDoc(JsonDocument& doc) {
  const Config& cfg = getConfig();

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
}

// Формирует компактный JSON с датой и текущим солнечным режимом.
static void populateDateDoc(JsonDocument& doc) {
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
    JsonDocument doc;
    populateSettingsDoc(doc);
    sendJsonDocument(request, 200, doc);
  });

  server.on("/api/date", HTTP_GET, [](AsyncWebServerRequest *request){
    metrics_record_http_request();
    JsonDocument doc;
    populateDateDoc(doc);
    sendJsonDocument(request, 200, doc, true);
  });

  server.on("/api/sun", HTTP_GET, [](AsyncWebServerRequest* request) {
    metrics_record_http_request();

    const Config& cfg = getConfig();
    double latitude = cfg.location.lat;
    double longitude = cfg.location.lng;

    if (request->hasParam("lat") || request->hasParam("lat", true)) {
      if (!parseCoordinateParameter(request, "lat", latitude)) {
        sendStatusMessage(request, 400, "error", "invalid lat");
        return;
      }
    }

    if (request->hasParam("lon") || request->hasParam("lon", true)) {
      if (!parseCoordinateParameter(request, "lon", longitude)) {
        sendStatusMessage(request, 400, "error", "invalid lon");
        return;
      }
    } else if (request->hasParam("lng") || request->hasParam("lng", true)) {
      if (!parseCoordinateParameter(request, "lng", longitude)) {
        sendStatusMessage(request, 400, "error", "invalid lng");
        return;
      }
    }

    if (latitude < -90.0 || latitude > 90.0 || longitude < -180.0 || longitude > 180.0) {
      sendStatusMessage(request, 400, "error", "coordinates out of range");
      return;
    }

    const time_t nowUtc = time(nullptr);
    const bool hasValidTime = ntp_has_valid_time();
    const int tzOffsetMinutes = ntp_utc_offset_minutes();

    struct tm localTm {};
    if (localtime_r(&nowUtc, &localTm) == nullptr) {
      sendStatusMessage(request, 500, "error", "failed to resolve local time");
      return;
    }

    SunOfflinePosition currentPosition {};
    if (!sun_offline_calculate_position_utc(nowUtc, latitude, longitude, currentPosition)) {
      sendStatusMessage(request, 500, "error", "failed to calculate current sun position");
      return;
    }

    SunOfflineEvents events {};
    if (!sun_offline_calculate_events_local_day(
            localTm.tm_year + 1900,
            localTm.tm_mon + 1,
            localTm.tm_mday,
            latitude,
            longitude,
            tzOffsetMinutes,
            events)) {
          sendStatusMessage(request, 500, "error", "failed to calculate sun events");
      return;
    }

    const uint16_t minuteOfDay = static_cast<uint16_t>(localTm.tm_hour * 60 + localTm.tm_min);
    const SunOfflineMode currentMode = sun_offline_mode_from_events(minuteOfDay, events);

    struct tm midnightLocal = localTm;
    midnightLocal.tm_hour = 0;
    midnightLocal.tm_min = 0;
    midnightLocal.tm_sec = 0;
    const time_t midnightEpoch = mktime(&midnightLocal);

    JsonDocument doc;
    doc["status"] = "ok";
    doc["validTime"] = hasValidTime;
    doc["timestamp"] = static_cast<uint32_t>(nowUtc);
    doc["timezoneOffsetMinutes"] = tzOffsetMinutes;
    doc["sunMode"] = sun_offline_mode_name(currentMode);

    JsonObject location = doc["location"].to<JsonObject>();
    location["lat"] = latitude;
    location["lon"] = longitude;

    JsonObject date = doc["date"].to<JsonObject>();
    date["year"] = localTm.tm_year + 1900;
    date["month"] = localTm.tm_mon + 1;
    date["day"] = localTm.tm_mday;

    JsonObject now = doc["now"].to<JsonObject>();
    now["minuteOfDay"] = minuteOfDay;
    now["azimuth"] = currentPosition.azimuth_deg;
    now["elevation"] = currentPosition.elevation_deg;
    now["zenith"] = currentPosition.zenith_deg;
    now["declination"] = currentPosition.declination_deg;
    now["equationOfTime"] = currentPosition.equation_of_time_min;

    JsonObject eventsObj = doc["events"].to<JsonObject>();
    eventsObj["hasSunrise"] = events.has_sunrise;
    eventsObj["hasSunset"] = events.has_sunset;
    eventsObj["hasCivilTwilight"] = events.has_civil_twilight;
    eventsObj["hasNauticalTwilight"] = events.has_nautical_twilight;
    eventsObj["hasAstronomicalTwilight"] = events.has_astronomical_twilight;
    eventsObj["isPolarDay"] = events.is_polar_day;
    eventsObj["isPolarNight"] = events.is_polar_night;
    eventsObj["sunrise"] = events.sunrise_minute;
    eventsObj["sunset"] = events.sunset_minute;
    eventsObj["civilBegin"] = events.civil_begin_minute;
    eventsObj["civilEnd"] = events.civil_end_minute;
    eventsObj["nauticalBegin"] = events.nautical_begin_minute;
    eventsObj["nauticalEnd"] = events.nautical_end_minute;
    eventsObj["astronomicalBegin"] = events.astronomical_begin_minute;
    eventsObj["astronomicalEnd"] = events.astronomical_end_minute;

    JsonArray path = doc["path"].to<JsonArray>();
    for (uint16_t minute = 0; minute < 1440; minute += 10) {
      if ((minute % 60U) == 0U) {
        delay(0);
      }

      SunOfflinePosition sample {};
      const time_t sampleEpoch = midnightEpoch + static_cast<time_t>(minute) * 60;

      if (!sun_offline_calculate_position_utc(sampleEpoch, latitude, longitude, sample)) {
        continue;
      }

      JsonObject point = path.add<JsonObject>();
      point["minute"] = minute;
      point["azimuth"] = static_cast<float>(sample.azimuth_deg);
      point["elevation"] = static_cast<float>(sample.elevation_deg);
      point["mode"] = sun_offline_mode_name(sun_offline_mode_from_elevation(sample.elevation_deg));
    }
    if (doc.overflowed()) {
      sendStatusMessage(request, 503, "error", "insufficient memory for sun payload");
      return;
    }

    sendJsonDocument(request, 200, doc, true);
  });

  server.on("/api/moon", HTTP_GET, [](AsyncWebServerRequest* request) {
    metrics_record_http_request();

    const bool hasValidTime = ntp_has_valid_time();
    const time_t nowUtc = time(nullptr);

    MoonPhase mp {};
    if (!moon_phase_calculate(nowUtc, mp)) {
      sendStatusMessage(request, 503, "error", "time not available");
      return;
    }

    JsonDocument doc;
    doc["status"]       = "ok";
    doc["validTime"]    = hasValidTime;
    doc["timestamp"]    = static_cast<uint32_t>(nowUtc);
    doc["ageDays"]      = mp.age_days;
    doc["illumination"] = mp.illumination;
    doc["phase"]        = static_cast<uint8_t>(mp.phase);
    doc["phaseName"]    = moon_phase_name(mp.phase);

    sendJsonDocument(request, 200, doc, true);
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
      sendStatusMessage(request, 500, "error", "request buffer allocation failed");
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
      sendStatusMessage(request, 400, "error", "invalid JSON payload");
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
      sendStatusMessage(request, 500, "error", "failed to save settings");
      return;
    }

    const bool outputOn = cfg.location.enabled ? sun_position_should_enable_now() : cfg.candleOn;
    i2c_set_brightness(outputOn ? cfg.brightness : 0);
    JsonDocument docResponse;
    docResponse["status"] = "ok";
    docResponse["restarting"] = true;
    docResponse["message"] = "settings saved, device restarting";
    sendJsonDocument(request, 200, docResponse);
    scheduleDeviceRestart(750);
  });

  server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest *request){
    metrics_record_http_request();
    configResetToDefault();

    if (!configSave()) {
      sendStatusMessage(request, 500, "error", "failed to clear Wi-Fi credentials");
      return;
    }

    i2c_set_brightness(getConfig().brightness);
    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Wi-Fi credentials cleared, device restarting";
    sendJsonDocument(request, 200, doc);
    scheduleDeviceRestart(750);
  });

  server.on("/api/brightness", HTTP_ANY, [](AsyncWebServerRequest *request){
    metrics_record_http_request();

    if (request->method() == HTTP_GET) {
      JsonDocument doc;
      doc["status"] = "ok";
      doc["brightness"] = getConfig().brightness;
      sendJsonDocument(request, 200, doc);
      return;
    }

    uint8_t value = 0;
    if (!parseBrightnessValue(request, value)) {
      sendStatusMessage(request, 400, "error", "brightness must be an integer percent from 0 to 100");
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
      sendStatusMessage(request, 500, "error", "failed to save brightness");
      return;
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["brightness"] = value;
    doc["candleOn"] = cfg.candleOn;
    doc["autoMode"] = cfg.location.enabled;
    doc["autoCandleOn"] = autoCandleOn;
    doc["sunMode"] = sun_position_current_mode_name();
    sendJsonDocument(request, 200, doc);
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
