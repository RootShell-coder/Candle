#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <stdlib.h>

#include "config.h"

static Config currentConfig;

template <size_t N>
// Копирует строку из JSON в буфер конфигурации с запасным значением.
void copyConfigString(char (&dest)[N], JsonVariantConst value, const char* fallback) {
  const char* src = fallback != nullptr ? fallback : "";

  if (value.is<const char*>()) {
    const char* jsonValue = value.as<const char*>();
    if (jsonValue != nullptr) {
      src = jsonValue;
    }
  }

  strlcpy(dest, src, N);
}

template <typename T>
// Ограничивает значение конфигурации заданным диапазоном.
T clampConfigValue(T value, T minValue, T maxValue) {
  return value < minValue ? minValue : (value > maxValue ? maxValue : value);
}

// Проверяет корректность широты для модуля расписания солнца.
bool isValidLatitude(double value) {
  return value >= -90.0 && value <= 90.0;
}

// Проверяет корректность долготы для модуля расписания солнца.
bool isValidLongitude(double value) {
  return value >= -180.0 && value <= 180.0;
}

// Извлекает координату из JSON, принимая и строку с точкой/запятой, и число.
bool parseCoordinateValue(JsonVariantConst value, double& result) {
  if (value.is<float>() || value.is<double>() ||
      value.is<int>() || value.is<long>() ||
      value.is<unsigned int>() || value.is<unsigned long>()) {
    result = value.as<double>();
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

  result = parsed;
  return true;
}

// Заполняет сетевые параметры безопасными значениями по умолчанию.
static void setDefaultWiFiConfig(WiFiConfig& wifi) {
  strlcpy(wifi.devname, "candle_light", sizeof(wifi.devname));
  strlcpy(wifi.name, "Candle Light", sizeof(wifi.name));
  strlcpy(wifi.ssid, "", sizeof(wifi.ssid));
  strlcpy(wifi.password, "", sizeof(wifi.password));
  wifi.power = 0;
  strlcpy(wifi.phy_mode, "11n", sizeof(wifi.phy_mode));
}

// Восстанавливает полный набор заводских настроек приложения.
static void setDefaultConfig() {
  memset(&currentConfig, 0, sizeof(currentConfig));
  setDefaultWiFiConfig(currentConfig.wifi);

  currentConfig.ota.port = 3232;
  strlcpy(currentConfig.ota.hostname, currentConfig.wifi.devname, sizeof(currentConfig.ota.hostname));

  strlcpy(currentConfig.ntp.ntp_server, "pool.ntp.org", sizeof(currentConfig.ntp.ntp_server));
  strlcpy(currentConfig.ntp.ntp_timezone, "Europe/London", sizeof(currentConfig.ntp.ntp_timezone));

  currentConfig.location.enabled = true;
  currentConfig.location.lat = 51.5287398;
  currentConfig.location.lng = -0.2664056;
  currentConfig.brightness = 16;
  currentConfig.candleOn = true;
}

// Очищает Wi‑Fi-учётные данные перед повторной настройкой устройства.
void configResetToDefault() {
  currentConfig.wifi.ssid[0] = '\0';
  currentConfig.wifi.password[0] = '\0';
}

// Возвращает текущую конфигурацию только для чтения.
const Config& getConfig() {
  return currentConfig;
}

// Возвращает изменяемую конфигурацию для внутренних модулей.
Config& getMutableConfig() {
  return currentConfig;
}

// Обновляет яркость в процентах 0..100 и синхронизирует флаг включения свечи.
bool configSetBrightness(uint8_t brightness) {
  brightness = brightness > 100 ? 100 : brightness;

  if (currentConfig.brightness == brightness) {
    if (brightness == 0) {
      currentConfig.candleOn = false;
    }
    return true;
  }

  currentConfig.brightness = brightness;
  if (brightness == 0) {
    currentConfig.candleOn = false;
  }
  return configSave();
}

// Инициализирует LittleFS и при необходимости форматирует раздел.
static bool initLittleFS() {
  // basePath = "/littlefs", partitionLabel = "littlefs".
  if (LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
    return true;
  }

  Serial.println("[config] LittleFS mount failed, trying format...");
  if (!LittleFS.format()) {
    Serial.println("[config] LittleFS format failed");
    return false;
  }

  if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
    Serial.println("[config] LittleFS mount still failed after format");
    return false;
  }

  return true;
}

// Сохраняет текущую конфигурацию в JSON-файл.
bool configSave() {
  if (!initLittleFS()) {
    Serial.println("[config] LittleFS init failed in save");
    return false;
  }

  File file = LittleFS.open(CONFIG_JSON_PATH, "w");
  if (!file) {
    Serial.println("[config] Failed to open config file for writing");
    return false;
  }

  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();

  JsonObject wifi = root["wifi"].to<JsonObject>();
  wifi["devname"] = currentConfig.wifi.devname;
  wifi["name"] = currentConfig.wifi.name;
  wifi["ssid"] = currentConfig.wifi.ssid;
  wifi["password"] = currentConfig.wifi.password;
  wifi["power"] = currentConfig.wifi.power;
  wifi["phy_mode"] = currentConfig.wifi.phy_mode;

  JsonObject ota = root["OTA"].to<JsonObject>();
  ota["port"] = currentConfig.ota.port;
  ota["hostname"] = currentConfig.ota.hostname;

  JsonObject ntp = root["ntp"].to<JsonObject>();
  ntp["ntp_server"] = currentConfig.ntp.ntp_server;
  ntp["ntp_timezone"] = currentConfig.ntp.ntp_timezone;

  JsonObject location = root["location"].to<JsonObject>();
  location["enabled"] = currentConfig.location.enabled;
  location["lat"] = currentConfig.location.lat;
  location["lng"] = currentConfig.location.lng;

  root["brightness"] = currentConfig.brightness;
  root["candleOn"] = currentConfig.candleOn;

  if (serializeJsonPretty(doc, file) == 0) {
    Serial.println("[config] Failed to serialize config");
    file.close();
    return false;
  }

  file.close();
  return true;
}

// Загружает настройки из LittleFS и нормализует значения по диапазонам.
bool configLoad() {
  setDefaultConfig();

  if (!initLittleFS()) {
    Serial.println("[config] LittleFS init failed");
    return false;
  }

  Serial.printf(
      "[config] Mounted FS, path=%s, exists=%d\n",
      CONFIG_JSON_PATH,
      LittleFS.exists(CONFIG_JSON_PATH));

  if (!LittleFS.exists(CONFIG_JSON_PATH)) {
    Serial.println("[config] settings file not found, writing default");
    return configSave();
  }

  File file = LittleFS.open(CONFIG_JSON_PATH, "r");
  if (!file) {
    Serial.printf("[config] Failed to open config file: %s\n", CONFIG_JSON_PATH);
    return false;
  }

  if (file.size() == 0) {
    Serial.println("[config] config file is empty, restoring defaults");
    file.close();
    return configSave();
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    Serial.print("[config] deserializeJson failed: ");
    Serial.println(error.c_str());
    return configSave();
  }

  const JsonObject wifi = doc["wifi"].as<JsonObject>();
  const JsonObject ota = doc["OTA"].as<JsonObject>();
  const JsonObject ntp = doc["ntp"].as<JsonObject>();
  const JsonObject location = doc["location"].as<JsonObject>();

  copyConfigString(currentConfig.wifi.devname, wifi["devname"], currentConfig.wifi.devname);
  copyConfigString(currentConfig.wifi.name, wifi["name"], currentConfig.wifi.name);
  copyConfigString(currentConfig.wifi.ssid, wifi["ssid"], currentConfig.wifi.ssid);
  copyConfigString(currentConfig.wifi.password, wifi["password"], currentConfig.wifi.password);
  currentConfig.wifi.power = clampConfigValue<int>(wifi["power"] | currentConfig.wifi.power, 0, 84);
  copyConfigString(currentConfig.wifi.phy_mode, wifi["phy_mode"], currentConfig.wifi.phy_mode);

  currentConfig.ota.port = clampConfigValue<int>(ota["port"] | currentConfig.ota.port, 0, 65535);
  copyConfigString(currentConfig.ota.hostname, ota["hostname"], currentConfig.wifi.devname);

  copyConfigString(currentConfig.ntp.ntp_server, ntp["ntp_server"], currentConfig.ntp.ntp_server);
  if (ntp["ntp_timezone"].is<const char*>()) {
    copyConfigString(currentConfig.ntp.ntp_timezone, ntp["ntp_timezone"], currentConfig.ntp.ntp_timezone);
  } else if (!ntp["ntp_timezone"].isNull()) {
    snprintf(
        currentConfig.ntp.ntp_timezone,
        sizeof(currentConfig.ntp.ntp_timezone),
        "%ld",
        ntp["ntp_timezone"] | 0L);
  }

  currentConfig.location.enabled = location["enabled"] | currentConfig.location.enabled;

  double lat = currentConfig.location.lat;
  if (parseCoordinateValue(location["lat"], lat)) {
    if (isValidLatitude(lat)) {
      currentConfig.location.lat = lat;
    } else {
      Serial.printf("[config] ignoring invalid latitude: %.6f\n", lat);
    }
  }

  double lng = currentConfig.location.lng;
  if (parseCoordinateValue(location["lng"], lng)) {
    if (isValidLongitude(lng)) {
      currentConfig.location.lng = lng;
    } else {
      Serial.printf("[config] ignoring invalid longitude: %.6f\n", lng);
    }
  }

  int storedBrightness = clampConfigValue<int>(
      doc["brightness"] | static_cast<int>(currentConfig.brightness), 0, 255);
  if (storedBrightness > 100) {
    storedBrightness = (storedBrightness * 100 + 127) / 255;
  }
  currentConfig.brightness = static_cast<uint8_t>(clampConfigValue<int>(storedBrightness, 0, 100));
  currentConfig.candleOn = doc["candleOn"] | (currentConfig.brightness > 0);

  if (currentConfig.brightness == 0) {
    currentConfig.candleOn = false;
  }

  if (currentConfig.ota.hostname[0] == '\0') {
    strlcpy(currentConfig.ota.hostname, currentConfig.wifi.devname, sizeof(currentConfig.ota.hostname));
  }

  return true;
}
