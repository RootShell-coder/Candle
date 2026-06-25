#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdlib.h>

#include "config.h"
#include "wifi_module.h"

static Config currentConfig;
static SemaphoreHandle_t s_configMutex = nullptr;
static SemaphoreHandle_t s_configFileMutex = nullptr;

namespace {
constexpr const char* kConfigTmpPath = "/settings.tmp";
constexpr const char* kConfigBackupPath = "/settings.bak";

SemaphoreHandle_t configMutex() {
  if (s_configMutex == nullptr) {
    s_configMutex = xSemaphoreCreateMutex();
  }
  return s_configMutex;
}

SemaphoreHandle_t configFileMutex() {
  if (s_configFileMutex == nullptr) {
    s_configFileMutex = xSemaphoreCreateMutex();
  }
  return s_configFileMutex;
}

struct ScopedSemaphoreLock {
  SemaphoreHandle_t mutex;
  bool locked;

  ScopedSemaphoreLock(SemaphoreHandle_t target, TickType_t timeout)
      : mutex(target),
        locked(target != nullptr && xSemaphoreTake(target, timeout) == pdTRUE) {
  }

  ~ScopedSemaphoreLock() {
    if (locked && mutex != nullptr) {
      xSemaphoreGive(mutex);
    }
  }
};
}  // namespace

template <size_t N>
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

template <size_t N>
void sanitizeConfigString(char (&value)[N], const char* fallback) {
  char sanitized[N] {};
  size_t out = 0;

  for (size_t in = 0; in < N && value[in] != '\0' && out + 1 < N; ++in) {
    const unsigned char c = static_cast<unsigned char>(value[in]);
    if (c >= 0x20 && c <= 0x7E) {
      sanitized[out++] = static_cast<char>(c);
    }
  }

  if (out == 0 && fallback != nullptr) {
    strlcpy(value, fallback, N);
    return;
  }

  sanitized[out] = '\0';
  strlcpy(value, sanitized, N);
}

void sanitizeConfigStrings(Config& cfg) {
  sanitizeConfigString(cfg.devname, "candle-light");
  sanitizeConfigString(cfg.name, "Candle Light");
  sanitizeConfigString(cfg.ntp.ntp_server, "pool.ntp.org");
  sanitizeConfigString(cfg.ntp.ntp_server2, "");
  sanitizeConfigString(cfg.ntp.ntp_timezone, "UTC0");
}

template <typename T>
T clampConfigValue(T value, T minValue, T maxValue) {
  return value < minValue ? minValue : (value > maxValue ? maxValue : value);
}

bool isValidLatitude(double value) {
  return value >= -90.0 && value <= 90.0;
}

bool isValidLongitude(double value) {
  return value >= -180.0 && value <= 180.0;
}

uint16_t normalizeMinuteOfDay(int value, uint16_t fallback) {
  if (value < 0 || value >= 24 * 60) {
    return fallback;
  }
  return static_cast<uint16_t>(value);
}

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

static void setDefaultConfig() {
  memset(&currentConfig, 0, sizeof(currentConfig));

  strlcpy(currentConfig.devname, "candle-light", sizeof(currentConfig.devname));
  strlcpy(currentConfig.name, "Candle Light", sizeof(currentConfig.name));
  strlcpy(currentConfig.ntp.ntp_server, "pool.ntp.org", sizeof(currentConfig.ntp.ntp_server));
  currentConfig.ntp.ntp_server2[0] = '\0';
  strlcpy(currentConfig.ntp.ntp_timezone, "Europe/London", sizeof(currentConfig.ntp.ntp_timezone));

  currentConfig.location.enabled = true;
  currentConfig.location.lat = 51.5287398;
  currentConfig.location.lng = -0.2664056;
  currentConfig.moonLed.enabled = false;
  currentConfig.moonLed.maxBrightness = 25;
  currentConfig.moonLed.hue = 42;
  currentConfig.timeSchedule.enabled = false;
  currentConfig.timeSchedule.onMinute = 18U * 60U;
  currentConfig.timeSchedule.offMinute = 23U * 60U;
  currentConfig.brightness = 16;
  currentConfig.candleOn = true;
}

Config getConfig() {
  Config snapshot {};
  ScopedSemaphoreLock lock(configMutex(), portMAX_DELAY);
  if (!lock.locked) {
    Serial.println("[config] Failed to lock config for reading");
    return snapshot;
  }
  snapshot = currentConfig;
  return snapshot;
}

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

static void populateConfigJson(JsonObject root, const Config& cfg) {
  root["devname"] = cfg.devname;
  root["name"] = cfg.name;

  JsonObject ntp = root["ntp"].to<JsonObject>();
  ntp["ntp_server"] = cfg.ntp.ntp_server;
  ntp["ntp_server2"] = cfg.ntp.ntp_server2;
  ntp["ntp_timezone"] = cfg.ntp.ntp_timezone;

  JsonObject location = root["location"].to<JsonObject>();
  location["enabled"] = cfg.location.enabled;
  location["lat"] = cfg.location.lat;
  location["lng"] = cfg.location.lng;

  JsonObject moonLed = root["moonLed"].to<JsonObject>();
  moonLed["enabled"] = cfg.moonLed.enabled;
  moonLed["maxBrightness"] = cfg.moonLed.maxBrightness;
  moonLed["hue"] = cfg.moonLed.hue;

  JsonObject timeSchedule = root["timeSchedule"].to<JsonObject>();
  timeSchedule["enabled"] = cfg.timeSchedule.enabled;
  timeSchedule["onMinute"] = cfg.timeSchedule.onMinute;
  timeSchedule["offMinute"] = cfg.timeSchedule.offMinute;

  root["brightness"] = cfg.brightness;
  root["candleOn"] = cfg.candleOn;
}

static void applyNtpConfigJson(Config& cfg, JsonObjectConst ntp) {
  copyConfigString(cfg.ntp.ntp_server, ntp["ntp_server"], cfg.ntp.ntp_server);
  copyConfigString(cfg.ntp.ntp_server2, ntp["ntp_server2"], cfg.ntp.ntp_server2);
  if (ntp["ntp_timezone"].is<const char*>()) {
    copyConfigString(cfg.ntp.ntp_timezone, ntp["ntp_timezone"], cfg.ntp.ntp_timezone);
  } else if (!ntp["ntp_timezone"].isNull()) {
    snprintf(cfg.ntp.ntp_timezone, sizeof(cfg.ntp.ntp_timezone), "%ld", ntp["ntp_timezone"] | 0L);
  }
}

static void applyLocationConfigJson(Config& cfg, JsonObjectConst location) {
  cfg.location.enabled = location["enabled"] | cfg.location.enabled;

  double lat = cfg.location.lat;
  if (parseCoordinateValue(location["lat"], lat)) {
    if (isValidLatitude(lat)) {
      cfg.location.lat = lat;
    } else {
      Serial.printf("[config] ignoring invalid latitude: %.6f\n", lat);
    }
  }

  double lng = cfg.location.lng;
  if (parseCoordinateValue(location["lng"], lng)) {
    if (isValidLongitude(lng)) {
      cfg.location.lng = lng;
    } else {
      Serial.printf("[config] ignoring invalid longitude: %.6f\n", lng);
    }
  }
}

static void applyMoonLedConfigJson(Config& cfg, JsonObjectConst moonLed) {
  cfg.moonLed.enabled = moonLed["enabled"] | cfg.moonLed.enabled;
  cfg.moonLed.maxBrightness = static_cast<uint8_t>(clampConfigValue<int>(
      moonLed["maxBrightness"] | static_cast<int>(cfg.moonLed.maxBrightness), 0, 100));
  cfg.moonLed.hue = static_cast<uint16_t>(clampConfigValue<int>(
      moonLed["hue"] | static_cast<int>(cfg.moonLed.hue), 0, 360));
}

static void applyTimeScheduleConfigJson(Config& cfg, JsonObjectConst timeSchedule) {
  cfg.timeSchedule.enabled = timeSchedule["enabled"] | cfg.timeSchedule.enabled;
  cfg.timeSchedule.onMinute = normalizeMinuteOfDay(
      timeSchedule["onMinute"] | static_cast<int>(cfg.timeSchedule.onMinute),
      cfg.timeSchedule.onMinute);
  cfg.timeSchedule.offMinute = normalizeMinuteOfDay(
      timeSchedule["offMinute"] | static_cast<int>(cfg.timeSchedule.offMinute),
      cfg.timeSchedule.offMinute);
}

static void applyOutputConfigJson(Config& cfg, JsonDocument& doc) {
  int storedBrightness = clampConfigValue<int>(
      doc["brightness"] | static_cast<int>(cfg.brightness), 0, 255);
  if (storedBrightness > 100) {
    storedBrightness = (storedBrightness * 100 + 127) / 255;
  }
  cfg.brightness = static_cast<uint8_t>(clampConfigValue<int>(storedBrightness, 0, 100));
  cfg.candleOn = doc["candleOn"] | (cfg.brightness > 0);

  if (cfg.brightness == 0) {
    cfg.candleOn = false;
  }
}

static bool writeConfigToFile(const Config& cfg) {
  Config sanitizedConfig = cfg;
  sanitizeConfigStrings(sanitizedConfig);

  ScopedSemaphoreLock fileLock(configFileMutex(), pdMS_TO_TICKS(10000));
  if (!fileLock.locked) {
    Serial.println("[config] Failed to lock config file for writing");
    return false;
  }

  if (!initLittleFS()) {
    Serial.println("[config] LittleFS init failed in save");
    return false;
  }

  LittleFS.remove(kConfigTmpPath);

  File file = LittleFS.open(kConfigTmpPath, "w");
  if (!file) {
    Serial.println("[config] Failed to open temp config file for writing");
    return false;
  }

  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  populateConfigJson(root, sanitizedConfig);

  if (serializeJsonPretty(doc, file) == 0) {
    Serial.println("[config] Failed to serialize config");
    file.close();
    return false;
  }

  file.close();

  File verifyFile = LittleFS.open(kConfigTmpPath, "r");
  if (!verifyFile) {
    Serial.println("[config] Failed to reopen temp config for verification");
    LittleFS.remove(kConfigTmpPath);
    return false;
  }

  JsonDocument verifyDoc;
  const DeserializationError verifyError = deserializeJson(verifyDoc, verifyFile);
  verifyFile.close();
  if (verifyError) {
    Serial.printf("[config] temp config verification failed: %s\n", verifyError.c_str());
    LittleFS.remove(kConfigTmpPath);
    return false;
  }

  LittleFS.remove(kConfigBackupPath);
  const bool hadConfig = LittleFS.exists(CONFIG_JSON_PATH);
  if (hadConfig && !LittleFS.rename(CONFIG_JSON_PATH, kConfigBackupPath)) {
    Serial.println("[config] Failed to move current config to backup");
    LittleFS.remove(kConfigTmpPath);
    return false;
  }

  if (!LittleFS.rename(kConfigTmpPath, CONFIG_JSON_PATH)) {
    Serial.println("[config] Failed to promote temp config");
    LittleFS.remove(CONFIG_JSON_PATH);
    if (hadConfig) {
      LittleFS.rename(kConfigBackupPath, CONFIG_JSON_PATH);
    }
    LittleFS.remove(kConfigTmpPath);
    return false;
  }

  LittleFS.remove(kConfigBackupPath);
  return true;
}

bool configSave() {
  return writeConfigToFile(getConfig());
}

bool configUpdate(const Config& config, bool saveToFile) {
  Config sanitizedConfig = config;
  sanitizeConfigStrings(sanitizedConfig);

  if (saveToFile && !writeConfigToFile(sanitizedConfig)) {
    return false;
  }

  ScopedSemaphoreLock lock(configMutex(), pdMS_TO_TICKS(1000));
  if (!lock.locked) {
    Serial.println("[config] Failed to lock config for update");
    return false;
  }

  currentConfig = sanitizedConfig;
  return true;
}

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

  const bool hasLegacyWiFiConfig = !doc["wifi"].isNull();
  const bool needsConfigMigration = hasLegacyWiFiConfig ||
      doc["moonLed"].isNull() ||
      doc["moonLed"]["enabled"].isNull() ||
      doc["moonLed"]["maxBrightness"].isNull() ||
      doc["moonLed"]["hue"].isNull();
  if (hasLegacyWiFiConfig) {
    const JsonObject legacyWifi = doc["wifi"].as<JsonObject>();
    const char* legacySsid = legacyWifi["ssid"] | "";
    const char* legacyPassword = legacyWifi["password"] | "";
    if (legacySsid[0] != '\0' && !wifi_has_credentials()) {
      (void)wifi_save_credentials(legacySsid, legacyPassword);
    }
  }

  copyConfigString(currentConfig.devname, doc["devname"], currentConfig.devname);
  copyConfigString(currentConfig.name, doc["name"], currentConfig.name);
  applyNtpConfigJson(currentConfig, doc["ntp"].as<JsonObjectConst>());
  applyLocationConfigJson(currentConfig, doc["location"].as<JsonObjectConst>());
  applyMoonLedConfigJson(currentConfig, doc["moonLed"].as<JsonObjectConst>());
  applyTimeScheduleConfigJson(currentConfig, doc["timeSchedule"].as<JsonObjectConst>());
  applyOutputConfigJson(currentConfig, doc);
  sanitizeConfigStrings(currentConfig);

  if (needsConfigMigration) {
    Serial.println("[config] migrating settings file");
    return configSave();
  }

  return true;
}
