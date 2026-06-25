#include <Arduino.h>
#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "config.h"
#include "i2c.h"
#include "metrics.h"
#include "moon_led.h"
#include "moon_phase.h"
#include "ntp_module.h"
#include "sun_offline.h"
#include "sun_position.h"
#include "web_server.h"
#include "wifi_module.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.0.0"
#endif

#ifndef BUILD_GIT_COMMIT
#define BUILD_GIT_COMMIT "unknown"
#endif

#ifndef BUILD_DATE
#define BUILD_DATE "unknown"
#endif

static AsyncWebServer server(80);

namespace {
volatile bool s_restartScheduled = false;
uint32_t s_restartAtMs = 0;
char s_restartReason[32] = "";
constexpr size_t kSaveJsonMaxBytes = 4096;
constexpr uint32_t kUpdateRequestMagic = 0x434F5441UL;
constexpr uint16_t kSunPathSampleStepMinutes = 10;
constexpr size_t kSunPathSampleCount = 1440 / kSunPathSampleStepMinutes;
constexpr uint32_t kSunPathCacheMaxAgeMs = 30UL * 60UL * 60UL * 1000UL;
constexpr double kSunPathCoordinateEpsilon = 0.000001;
constexpr float kSunPathThresholdEpsilon = 0.001f;

enum class UpdateRequestStatus : uint8_t {
  Started = 1,
  Succeeded = 2,
  Failed = 3,
};

struct UpdateRequestState {
  uint32_t magic;
  UpdateRequestStatus status;
  bool failed;
  bool ownsUpdate;
  bool filesystemTarget;
  size_t maxSize;
  char error[160];
};

struct UpdateStateSnapshot {
  bool active;
  bool succeeded;
  bool failed;
  bool filesystemTarget;
  size_t maxSize;
  String error;
};

struct ScopedSemaphoreLock {
  SemaphoreHandle_t mutex;
  bool locked;

  ScopedSemaphoreLock(SemaphoreHandle_t target, TickType_t timeout)
      : mutex(target),
        locked(target == nullptr || xSemaphoreTake(target, timeout) == pdTRUE) {
  }

  ~ScopedSemaphoreLock() {
    if (locked && mutex != nullptr) {
      xSemaphoreGive(mutex);
    }
  }
};

class UpdateStateStore {
public:
  bool begin(bool filesystemTarget, size_t maxSize) {
    ScopedSemaphoreLock lock(mutex(), pdMS_TO_TICKS(1000));
    if (!lock.locked || m_state.active) {
      return false;
    }

    m_state.active = true;
    m_state.succeeded = false;
    m_state.failed = false;
    m_state.error = "";
    m_state.filesystemTarget = filesystemTarget;
    m_state.maxSize = maxSize;
    return true;
  }

  bool fail(const String& message) {
    ScopedSemaphoreLock lock(mutex(), pdMS_TO_TICKS(1000));
    const bool firstFailure = !m_state.failed;
    if (lock.locked) {
      m_state.failed = true;
      m_state.succeeded = false;
      m_state.error = message;
    }
    return firstFailure;
  }

  void succeed(bool filesystemTarget, size_t maxSize) {
    ScopedSemaphoreLock lock(mutex(), pdMS_TO_TICKS(1000));
    if (!lock.locked) {
      return;
    }

    m_state.succeeded = true;
    m_state.failed = false;
    m_state.error = "";
    m_state.filesystemTarget = filesystemTarget;
    m_state.maxSize = maxSize;
  }

  UpdateStateSnapshot snapshot() {
    ScopedSemaphoreLock lock(mutex(), pdMS_TO_TICKS(1000));
    return m_state;
  }

  void clearActive() {
    ScopedSemaphoreLock lock(mutex(), pdMS_TO_TICKS(1000));
    if (lock.locked) {
      m_state.active = false;
    }
  }

private:
  SemaphoreHandle_t mutex() {
    if (m_mutex == nullptr) {
      m_mutex = xSemaphoreCreateMutex();
    }
    return m_mutex;
  }

  SemaphoreHandle_t m_mutex = nullptr;
  UpdateStateSnapshot m_state {};
};

UpdateStateStore s_updateStore;

struct SunPathPoint {
  uint16_t minute;
  float azimuth;
  float elevation;
  SunOfflineMode mode;
};

struct SunPathExtraPoint {
  uint32_t secondOfDay;
  float azimuth;
  float elevation;
  SunOfflineMode mode;
};

struct SunPathCache {
  bool valid;
  int year;
  int month;
  int day;
  int tzOffsetMinutes;
  double latitude;
  double longitude;
  uint32_t generatedAtMs;
  size_t count;
  SunPathPoint points[kSunPathSampleCount];
};

SunPathCache s_sunPathCache {};

template <size_t N>
void copyJsonString(char (&dest)[N], JsonVariantConst value) {
  if (!value.is<const char*>()) {
    return;
  }

  const char* src = value.as<const char*>();
  if (src != nullptr) {
    strlcpy(dest, src, N);
  }
}

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

bool parseJsonHue(JsonVariantConst value, uint16_t& hue) {
  if (!(value.is<int>() || value.is<long>() || value.is<unsigned int>() || value.is<unsigned long>())) {
    return false;
  }

  const int parsed = value.as<int>();
  if (parsed < 0 || parsed > 360) {
    return false;
  }

  hue = static_cast<uint16_t>(parsed);
  return true;
}

bool parseJsonMinuteOfDay(JsonVariantConst value, uint16_t& minute) {
  if (!(value.is<int>() || value.is<long>() || value.is<unsigned int>() || value.is<unsigned long>())) {
    return false;
  }

  const long parsed = value.as<long>();
  if (parsed < 0 || parsed >= 24L * 60L) {
    return false;
  }

  minute = static_cast<uint16_t>(parsed);
  return true;
}

bool parseManualTimeText(const String& rawValue, uint32_t& epochUtc) {
  char digits[13] {};
  size_t digitCount = 0;

  for (size_t index = 0; index < rawValue.length(); ++index) {
    const char c = rawValue[index];
    if (c >= '0' && c <= '9') {
      if (digitCount >= 12) {
        return false;
      }
      digits[digitCount++] = c;
    }
  }

  if (digitCount != 12) {
    return false;
  }

  const int hour = (digits[0] - '0') * 10 + (digits[1] - '0');
  const int minute = (digits[2] - '0') * 10 + (digits[3] - '0');
  const int day = (digits[4] - '0') * 10 + (digits[5] - '0');
  const int month = (digits[6] - '0') * 10 + (digits[7] - '0');
  const int year = (digits[8] - '0') * 1000 +
      (digits[9] - '0') * 100 +
      (digits[10] - '0') * 10 +
      (digits[11] - '0');

  if (year < 2024 || year > 2100 ||
      month < 1 || month > 12 ||
      day < 1 || day > 31 ||
      hour < 0 || hour > 23 ||
      minute < 0 || minute > 59) {
    return false;
  }

  static constexpr uint8_t kMonthDays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  uint8_t maxDay = kMonthDays[month - 1];
  const bool leapYear = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
  if (month == 2 && leapYear) {
    maxDay = 29;
  }
  if (day > maxDay) {
    return false;
  }

  struct tm localTm {};
  localTm.tm_year = year - 1900;
  localTm.tm_mon = month - 1;
  localTm.tm_mday = day;
  localTm.tm_hour = hour;
  localTm.tm_min = minute;
  localTm.tm_sec = 0;
  localTm.tm_isdst = -1;

  const time_t parsed = mktime(&localTm);
  if (parsed == static_cast<time_t>(-1) ||
      parsed < static_cast<time_t>(1704067200ULL)) {
    return false;
  }

  epochUtc = static_cast<uint32_t>(parsed);
  return true;
}

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

const AsyncWebParameter* findRequestParam(AsyncWebServerRequest* request, const char* name, bool post) {
  if (request->hasParam(name, post)) {
    return request->getParam(name, post);
  }
  return nullptr;
}

const AsyncWebParameter* findRequestParam(AsyncWebServerRequest* request, const char* name) {
  if (const AsyncWebParameter* param = findRequestParam(request, name, false)) {
    return param;
  }
  if (const AsyncWebParameter* param = findRequestParam(request, name, true)) {
    return param;
  }
  return nullptr;
}

bool parseBooleanValue(AsyncWebServerRequest* request, const char* name, bool& value) {
  const AsyncWebParameter* param = findRequestParam(request, name);
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

void normalizeAutoModes(Config& cfg) {
  if (cfg.timeSchedule.enabled) {
    cfg.location.enabled = false;
  } else if (cfg.location.enabled) {
    cfg.timeSchedule.enabled = false;
  }
}

bool hasManualCandleControl(const Config& cfg, bool hasValidTime) {
  return (!cfg.location.enabled && !cfg.timeSchedule.enabled) || !hasValidTime;
}

uint8_t resolveCandleBrightness(const Config& cfg, bool hasValidTime, bool autoCandleOn) {
  const bool autoEffective = hasValidTime && (cfg.location.enabled || cfg.timeSchedule.enabled);
  const bool outputOn = autoEffective ? autoCandleOn : cfg.candleOn;
  return outputOn ? cfg.brightness : 0;
}

void populateMoonLedDoc(JsonObject moonLed, const Config& cfg) {
  moonLed["enabled"] = cfg.moonLed.enabled;
  moonLed["maxBrightness"] = cfg.moonLed.maxBrightness;
  moonLed["hue"] = cfg.moonLed.hue;
  moonLed["currentBrightness"] = moon_led_current_brightness();
  moonLed["hardwareEnabled"] = moon_led_hardware_enabled();
}

bool manualTimeAllowed(const NtpStatus& ntpStatus) {
  return !ntpStatus.ntpSynchronized;
}

void applyMoonLedJson(Config& cfg, JsonObjectConst moonLed) {
  if (moonLed.isNull()) {
    return;
  }

  bool enabled = cfg.moonLed.enabled;
  if (parseJsonBool(moonLed["enabled"], enabled)) {
    cfg.moonLed.enabled = enabled;
  }

  uint8_t maxBrightness = cfg.moonLed.maxBrightness;
  if (parseJsonBrightness(moonLed["maxBrightness"], maxBrightness)) {
    cfg.moonLed.maxBrightness = maxBrightness;
  }

  uint16_t hue = cfg.moonLed.hue;
  if (parseJsonHue(moonLed["hue"], hue)) {
    cfg.moonLed.hue = hue;
  }
}

String printableJsonString(const char* value) {
  String out;
  if (value == nullptr) {
    return out;
  }

  while (*value != '\0') {
    const unsigned char c = static_cast<unsigned char>(*value++);
    if (c >= 0x20 && c <= 0x7E) {
      out += static_cast<char>(c);
    }
  }
  return out;
}

bool parseCoordinateParameter(AsyncWebServerRequest* request, const char* name, double& value) {
  const AsyncWebParameter* param = findRequestParam(request, name);
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

bool sunPathCacheMatches(
    int year,
    int month,
    int day,
    double latitude,
    double longitude,
    int tzOffsetMinutes) {
  if (!s_sunPathCache.valid || s_sunPathCache.count != kSunPathSampleCount) {
    return false;
  }

  if (s_sunPathCache.year != year ||
      s_sunPathCache.month != month ||
      s_sunPathCache.day != day ||
      s_sunPathCache.tzOffsetMinutes != tzOffsetMinutes) {
    return false;
  }

  if (fabs(s_sunPathCache.latitude - latitude) > kSunPathCoordinateEpsilon ||
      fabs(s_sunPathCache.longitude - longitude) > kSunPathCoordinateEpsilon) {
    return false;
  }

  return millis() - s_sunPathCache.generatedAtMs <= kSunPathCacheMaxAgeMs;
}

bool rebuildSunPathCache(
    int year,
    int month,
    int day,
    double latitude,
    double longitude,
    int tzOffsetMinutes,
    time_t midnightEpoch) {
  s_sunPathCache.valid = false;
  s_sunPathCache.year = year;
  s_sunPathCache.month = month;
  s_sunPathCache.day = day;
  s_sunPathCache.tzOffsetMinutes = tzOffsetMinutes;
  s_sunPathCache.latitude = latitude;
  s_sunPathCache.longitude = longitude;
  s_sunPathCache.generatedAtMs = millis();
  s_sunPathCache.count = kSunPathSampleCount;

  for (size_t index = 0; index < kSunPathSampleCount; ++index) {
    if ((index % 6U) == 0U) {
      delay(0);
    }

    const uint16_t minute = static_cast<uint16_t>(index * kSunPathSampleStepMinutes);
    const time_t sampleEpoch = midnightEpoch + static_cast<time_t>(minute) * 60;
    SunOfflinePosition sample {};

    if (!sun_offline_calculate_position_utc(sampleEpoch, latitude, longitude, sample)) {
      s_sunPathCache.valid = false;
      return false;
    }

    SunPathPoint& point = s_sunPathCache.points[index];
    point.minute = minute;
    point.azimuth = static_cast<float>(sample.azimuth_deg);
    point.elevation = static_cast<float>(sample.elevation_deg);
    point.mode = sun_offline_mode_from_elevation(sample.elevation_deg);
  }

  s_sunPathCache.valid = true;
  return true;
}

bool ensureSunPathCache(
    int year,
    int month,
    int day,
    double latitude,
    double longitude,
    int tzOffsetMinutes,
    time_t midnightEpoch) {
  if (sunPathCacheMatches(year, month, day, latitude, longitude, tzOffsetMinutes)) {
    return true;
  }

  return rebuildSunPathCache(year, month, day, latitude, longitude, tzOffsetMinutes, midnightEpoch);
}

bool calculateSunPathExtraPoint(
    uint32_t secondOfDay,
    float elevation,
    double latitude,
    double longitude,
    time_t midnightEpoch,
    SunPathExtraPoint& out) {
  if (secondOfDay >= 86400UL) {
    return false;
  }

  SunOfflinePosition position {};
  const time_t sampleEpoch = midnightEpoch + static_cast<time_t>(secondOfDay);
  if (!sun_offline_calculate_position_utc(sampleEpoch, latitude, longitude, position)) {
    return false;
  }

  out.secondOfDay = secondOfDay;
  out.azimuth = static_cast<float>(position.azimuth_deg);
  out.elevation = elevation;
  out.mode = sun_offline_mode_from_elevation(static_cast<double>(elevation));
  return true;
}

bool addSunPathExtraPoint(
    SunPathExtraPoint* points,
    size_t capacity,
    size_t& count,
    const SunPathExtraPoint& point) {
  if (count >= capacity) {
    return false;
  }

  for (size_t index = 0; index < count; ++index) {
    if (points[index].secondOfDay == point.secondOfDay &&
        fabs(points[index].elevation - point.elevation) < kSunPathThresholdEpsilon) {
      return true;
    }
  }

  points[count] = point;
  ++count;
  return true;
}

bool findSunPathThresholdCrossing(
    const SunPathPoint& from,
    const SunPathPoint& to,
    float threshold,
    double latitude,
    double longitude,
    time_t midnightEpoch,
    SunPathExtraPoint& out) {
  const float fromDelta = from.elevation - threshold;
  const float toDelta = to.elevation - threshold;

  if (fabs(fromDelta) < kSunPathThresholdEpsilon) {
    return calculateSunPathExtraPoint(
        static_cast<uint32_t>(from.minute) * 60UL,
        threshold,
        latitude,
        longitude,
        midnightEpoch,
        out);
  }

  if (fabs(toDelta) < kSunPathThresholdEpsilon || fromDelta * toDelta > 0.0f) {
    return false;
  }

  uint32_t lo = static_cast<uint32_t>(from.minute) * 60UL;
  uint32_t hi = static_cast<uint32_t>(to.minute) * 60UL;
  const bool rising = to.elevation > from.elevation;

  while (hi - lo > 1UL) {
    const uint32_t mid = lo + (hi - lo) / 2UL;
    SunOfflinePosition midPosition {};
    if (!sun_offline_calculate_position_utc(midnightEpoch + static_cast<time_t>(mid), latitude, longitude, midPosition)) {
      return false;
    }

    const bool midBelow = midPosition.elevation_deg < static_cast<double>(threshold);
    if (rising) {
      if (midBelow) {
        lo = mid;
      } else {
        hi = mid;
      }
    } else {
      if (midBelow) {
        hi = mid;
      } else {
        lo = mid;
      }
    }
  }

  return calculateSunPathExtraPoint(hi, threshold, latitude, longitude, midnightEpoch, out);
}

void sortSunPathExtraPoints(SunPathExtraPoint* points, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    SunPathExtraPoint value = points[i];
    size_t j = i;
    while (j > 0 && points[j - 1].secondOfDay > value.secondOfDay) {
      points[j] = points[j - 1];
      --j;
    }
    points[j] = value;
  }
}

bool scheduleDeviceRestart(uint32_t delayMs, const char* reason) {
  s_restartScheduled = true;
  s_restartAtMs = millis() + delayMs;
  strlcpy(s_restartReason, reason != nullptr ? reason : "requested", sizeof(s_restartReason));

  Serial.printf("[web] restart scheduled in %lu ms, reason=%s\n",
                static_cast<unsigned long>(delayMs),
                s_restartReason);
  return true;
}

uint32_t restartInMs() {
  if (!s_restartScheduled || s_restartAtMs == 0) {
    return 0;
  }

  const uint32_t now = millis();
  if (static_cast<int32_t>(s_restartAtMs - now) <= 0) {
    return 0;
  }
  return s_restartAtMs - now;
}

void setUpdateRequestError(UpdateRequestState* state, const String& message) {
  if (state == nullptr) {
    return;
  }

  state->failed = true;
  state->status = UpdateRequestStatus::Failed;
  strlcpy(state->error, message.c_str(), sizeof(state->error));
}

void failUpdate(UpdateRequestState* state, const String& message) {
  setUpdateRequestError(state, message);
  if ((state == nullptr || state->ownsUpdate) && s_updateStore.fail(message)) {
    metrics_record_ota_failure();
  }
  Serial.printf("[update] failed: %s\n", message.c_str());
}

size_t otaTargetPartitionSize(bool filesystemUpdate);

UpdateRequestState* updateRequestState(AsyncWebServerRequest* request) {
  UpdateRequestState* state = static_cast<UpdateRequestState*>(request->_tempObject);
  if (state == nullptr || state->magic != kUpdateRequestMagic) {
    return nullptr;
  }
  return state;
}

UpdateRequestState* ensureUpdateRequestState(AsyncWebServerRequest* request) {
  UpdateRequestState* state = updateRequestState(request);
  if (state != nullptr) {
    return state;
  }

  state = static_cast<UpdateRequestState*>(malloc(sizeof(UpdateRequestState)));
  if (state == nullptr) {
    if (s_updateStore.fail("failed to allocate update request state")) {
      metrics_record_ota_failure();
    }
    return nullptr;
  }

  state->magic = kUpdateRequestMagic;
  state->status = UpdateRequestStatus::Started;
  state->failed = false;
  state->ownsUpdate = false;
  state->filesystemTarget = false;
  state->maxSize = 0;
  state->error[0] = '\0';
  request->_tempObject = state;
  return state;
}

void destroyUpdateRequestState(AsyncWebServerRequest* request) {
  UpdateRequestState* state = updateRequestState(request);
  if (state == nullptr) {
    return;
  }

  free(state);
  request->_tempObject = nullptr;
}

bool beginUpdateRequest(AsyncWebServerRequest* request,
                        const String& filename,
                        bool filesystemUpdate,
                        size_t expectedSize,
                        bool exactSize) {
  UpdateRequestState* state = ensureUpdateRequestState(request);
  if (state == nullptr) {
    return false;
  }

  state->status = UpdateRequestStatus::Started;
  state->failed = false;
  state->ownsUpdate = false;
  state->filesystemTarget = filesystemUpdate;
  state->maxSize = otaTargetPartitionSize(filesystemUpdate);
  state->error[0] = '\0';

  const int command = filesystemUpdate ? U_SPIFFS : U_FLASH;
  Serial.printf(
      "[update] starting %s update from '%s', expected=%u\n",
      filesystemUpdate ? "filesystem" : "firmware",
      filename.c_str(),
      static_cast<unsigned>(expectedSize));

  if (filename.length() != 0 && !filename.endsWith(".bin")) {
    failUpdate(state, "update file must be a .bin binary");
  }

  if (exactSize && expectedSize == 0) {
    failUpdate(state, "update file is empty");
  }

  if (!state->failed && state->maxSize == 0) {
    failUpdate(state, "target update partition not found");
  }

  if (!state->failed && expectedSize > 0) {
    const size_t allowedSize = exactSize ? state->maxSize : state->maxSize + 4096U;
    if (expectedSize > allowedSize) {
      String err = "update upload is larger than ";
      err += filesystemUpdate ? "filesystem" : "firmware";
      err += " partition: upload=";
      err += String(static_cast<unsigned>(expectedSize));
      err += " partition=";
      err += String(static_cast<unsigned>(state->maxSize));
      failUpdate(state, err);
    }
  }

  if (!state->failed && !s_updateStore.begin(filesystemUpdate, state->maxSize)) {
    failUpdate(state, "another update is already in progress");
  } else if (!state->failed) {
    state->ownsUpdate = true;
  }

  if (!state->failed && state->ownsUpdate) {
    metrics_set_ota_active(true);
  }

  if (!state->failed && !Update.begin(state->maxSize, command)) {
    String err = "failed to begin update";
    if (Update.hasError()) {
      err += ": ";
      err += Update.errorString();
    }
    failUpdate(state, err);
  }

  return !state->failed;
}

void writeUpdateChunk(UpdateRequestState* state, size_t index, uint8_t* data, size_t len) {
  if (state == nullptr || state->failed || len == 0) {
    return;
  }

  if (state->maxSize != 0 && index + len > state->maxSize) {
    String err = "update file is larger than target partition: written=";
    err += String(static_cast<unsigned>(index + len));
    err += " partition=";
    err += String(static_cast<unsigned>(state->maxSize));
    failUpdate(state, err);
    return;
  }

  const size_t written = Update.write(data, len);
  if (written != len) {
    String err = "failed to write update chunk";
    if (Update.hasError()) {
      err += ": ";
      err += Update.errorString();
    }
    failUpdate(state, err);
  }
}

void finishUpdateRequest(UpdateRequestState* state, const String& filename, size_t totalWritten) {
  if (state == nullptr) {
    return;
  }

  if (!state->failed) {
    if (!Update.end(true)) {
      String err = "failed to finalize update";
      if (Update.hasError()) {
        err += ": ";
        err += Update.errorString();
      }
      failUpdate(state, err);
    } else {
      s_updateStore.succeed(state->filesystemTarget, state->maxSize);
      metrics_record_ota_success();
      state->status = UpdateRequestStatus::Succeeded;
      Serial.printf("[update] uploaded %u bytes from '%s'\n",
                    static_cast<unsigned>(totalWritten),
                    filename.c_str());
    }
  } else if (state->ownsUpdate) {
    Update.abort();
    state->status = UpdateRequestStatus::Failed;
  }

  if (state->failed) {
    state->status = UpdateRequestStatus::Failed;
  }

  if (state->ownsUpdate) {
    s_updateStore.clearActive();
    metrics_set_ota_active(false);
  }
}

size_t otaTargetPartitionSize(bool filesystemUpdate) {
  const esp_partition_t* partition = nullptr;
  if (filesystemUpdate) {
    partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
        "littlefs");
    if (partition == nullptr) {
      partition = esp_partition_find_first(
          ESP_PARTITION_TYPE_DATA,
          ESP_PARTITION_SUBTYPE_ANY,
          "littlefs");
    }
  } else {
    partition = esp_ota_get_next_update_partition(nullptr);
  }

  return partition != nullptr ? partition->size : 0;
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

static void sendUpdateResult(AsyncWebServerRequest* request) {
  if (request->getResponse() != nullptr) {
    return;
  }

  JsonDocument doc;
  const UpdateRequestState* updateState = updateRequestState(request);
  const UpdateStateSnapshot updateSnapshot = s_updateStore.snapshot();
  if (updateState == nullptr) {
    doc["status"] = "error";
    doc["message"] = "no update file uploaded";
    sendJsonDocument(request, 400, doc, true);
    return;
  }

  if (updateState->status != UpdateRequestStatus::Succeeded ||
      !updateSnapshot.succeeded ||
      updateSnapshot.failed ||
      Update.hasError()) {
    doc["status"] = "error";
    doc["message"] = updateState->error[0] != '\0'
        ? updateState->error
        : (updateSnapshot.error.length() != 0 ? updateSnapshot.error : "update failed");
    sendJsonDocument(request, 500, doc, true);
    destroyUpdateRequestState(request);
    return;
  }

  doc["status"] = "ok";
  doc["restarting"] = false;
  doc["message"] = "update uploaded, restart deferred";
  sendJsonDocument(request, 200, doc, true);
  destroyUpdateRequestState(request);
}

static void redirectToCaptivePortal(AsyncWebServerRequest* request) {
  request->redirect(String("http://") + WiFi.softAPIP().toString() + "/");
}

static void populateSettingsDoc(JsonDocument& doc) {
  const Config& cfg = getConfig();
  const NtpStatus ntpStatus = ntp_status();
  WiFiCredentials wifiCredentials {};
  const bool hasWifiCredentials = wifi_get_credentials(wifiCredentials);

  doc["devname"] = printableJsonString(cfg.devname);
  doc["name"] = printableJsonString(cfg.name);
  doc["firmwareVersion"] = FIRMWARE_VERSION;
  doc["buildCommit"] = BUILD_GIT_COMMIT;
  doc["buildDate"] = BUILD_DATE;
  doc["ssid"] = hasWifiCredentials ? printableJsonString(wifiCredentials.ssid) : "";
  doc["password"] = "";
  doc["ntp_server"] = printableJsonString(cfg.ntp.ntp_server);
  doc["ntp_server2"] = printableJsonString(cfg.ntp.ntp_server2);
  doc["ntp_timezone"] = printableJsonString(cfg.ntp.ntp_timezone);
  doc["validTime"] = ntpStatus.validTime;
  doc["ntpSynchronized"] = ntpStatus.ntpSynchronized;
  doc["manualTimeAllowed"] = manualTimeAllowed(ntpStatus);
  doc["wifiStaConnected"] = ntpStatus.wifiConnected;
  doc["wifiHasIp"] = ntpStatus.hasIp;
  doc["lat"] = cfg.location.lat;
  doc["lon"] = cfg.location.lng;
  doc["autoMode"] = cfg.location.enabled;
  doc["autoTimeMode"] = cfg.timeSchedule.enabled;
  doc["timeOnMinute"] = cfg.timeSchedule.onMinute;
  doc["timeOffMinute"] = cfg.timeSchedule.offMinute;
  doc["sunMode"] = sun_position_current_mode_name();
  doc["autoCandleOn"] = sun_position_should_enable_now();
  doc["autoTimeCandleOn"] = sun_position_time_schedule_should_enable_now();
  doc["brightness"] = cfg.brightness;
  doc["candleOn"] = cfg.candleOn;

  populateMoonLedDoc(doc["moonLed"].to<JsonObject>(), cfg);
}

static void populateDateDoc(JsonDocument& doc) {
  char formatted[24] = "--:-- --.--.----";
  bool valid = false;
  const NtpStatus ntpStatus = ntp_status();

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
  doc["autoTimeCandleOn"] = sun_position_time_schedule_should_enable_now();
  doc["candleOn"] = sun_position_is_candle_enabled();

  JsonObject ntp = doc["ntp"].to<JsonObject>();
  ntp["wifiConnected"] = ntpStatus.wifiConnected;
  ntp["hasIp"] = ntpStatus.hasIp;
  ntp["syncInProgress"] = ntpStatus.syncInProgress;
  ntp["validTime"] = ntpStatus.validTime;
  ntp["ntpSynchronized"] = ntpStatus.ntpSynchronized;
  ntp["manualTimeAllowed"] = manualTimeAllowed(ntpStatus);
  ntp["bootSyncPending"] = ntpStatus.bootSyncPending;
  ntp["dnsResolved"] = ntpStatus.dnsResolved;
  ntp["failureStreak"] = ntpStatus.failureStreak;
  ntp["sntpSyncStatus"] = ntpStatus.sntpSyncStatus;
  ntp["nextSyncInMs"] = ntpStatus.nextSyncInMs;
  ntp["syncTimeoutInMs"] = ntpStatus.syncTimeoutInMs;
  ntp["lastSuccessEpoch"] = ntpStatus.lastSuccessEpoch;
  ntp["server"] = printableJsonString(ntpStatus.server);
  ntp["serverIp"] = printableJsonString(ntpStatus.serverIp);
  ntp["timezone"] = printableJsonString(ntpStatus.timezone);

  WiFiCredentials wifiCredentials {};
  const bool hasWifiCredentials = wifi_get_credentials(wifiCredentials);
  const bool staConnected = WiFi.status() == WL_CONNECTED;
  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["staConnected"] = staConnected;
  wifi["setupApActive"] = wifi_is_captive_portal_active();
  wifi["ssid"] = hasWifiCredentials ? printableJsonString(wifiCredentials.ssid) : "";
  wifi["ip"] = staConnected ? WiFi.localIP().toString() : "";
  wifi["rssi"] = staConnected ? WiFi.RSSI() : 0;
  wifi["setupSsid"] = "candle-setup";
  wifi["setupIp"] = wifi_is_captive_portal_active() ? WiFi.softAPIP().toString() : "";
}

static void handleManualTimeRequest(AsyncWebServerRequest* request, const String& value) {
  const NtpStatus ntpStatus = ntp_status();
  if (ntpStatus.ntpSynchronized) {
    sendStatusMessage(request, 409, "error", "time is already synchronized by NTP");
    return;
  }

  if (!manualTimeAllowed(ntpStatus)) {
    sendStatusMessage(request, 409, "error", "manual time is available only before NTP synchronization");
    return;
  }

  uint32_t epochUtc = 0;
  if (!parseManualTimeText(value, epochUtc)) {
    sendStatusMessage(request, 400, "error", "time format must be HH:MM DD.MM.YYYY");
    return;
  }

  if (!ntp_set_manual_time(epochUtc)) {
    sendStatusMessage(request, 500, "error", "failed to set manual time");
    return;
  }

  JsonDocument doc;
  doc["status"] = "ok";
  doc["validTime"] = ntp_has_valid_time();
  doc["ntpSynchronized"] = ntp_is_synchronized();
  populateDateDoc(doc);
  const NtpStatus updatedNtpStatus = ntp_status();
  doc["manualTimeAllowed"] = manualTimeAllowed(updatedNtpStatus);
  doc["wifiStaConnected"] = updatedNtpStatus.wifiConnected;
  doc["wifiHasIp"] = updatedNtpStatus.hasIp;
  sendJsonDocument(request, 200, doc, true);
}

static bool parseBrightnessValue(AsyncWebServerRequest* request, uint8_t& value) {
  const AsyncWebParameter* param = findRequestParam(request, "value", false);
  if (param == nullptr) {
    param = findRequestParam(request, "brightness", false);
  }
  if (param == nullptr) {
    param = findRequestParam(request, "value", true);
  }
  if (param == nullptr) {
    param = findRequestParam(request, "brightness", true);
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

static bool parseMinuteValue(AsyncWebServerRequest* request, const char* name, uint16_t& value) {
  const AsyncWebParameter* param = nullptr;
  if (request->hasParam(name)) {
    param = request->getParam(name);
  } else if (request->hasParam(name, true)) {
    param = request->getParam(name, true);
  }

  if (param == nullptr) {
    return false;
  }

  const int parsed = param->value().toInt();
  if (parsed < 0 || parsed >= 24 * 60) {
    return false;
  }
  value = static_cast<uint16_t>(parsed);
  return true;
}

void web_server_setup_with_wifi() {
  if (WiFi.status() != WL_CONNECTED && !wifi_is_captive_portal_active()) {
    if (!wifi_connect()) {
      Serial.println("[web] WiFi and setup AP are not ready, web server disabled");
      return;
    }
  }

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    metrics_record_http_request();
    if (LittleFS.exists("/index.html")) {
      request->send(LittleFS, "/index.html", "text/html");
    } else {
      request->send(200, "text/plain", "index.html not found");
    }
  });

  server.serveStatic("/index.html", LittleFS, "/index.html")
      .setDefaultFile("index.html")
      .setCacheControl("no-store, max-age=0");

  const auto captiveProbeHandler = [](AsyncWebServerRequest* request) {
    metrics_record_http_request();
    if (wifi_is_captive_portal_active()) {
      redirectToCaptivePortal(request);
      return;
    }

    request->send(204, "text/plain", "");
  };

  server.on("/generate_204", HTTP_GET, captiveProbeHandler);
  server.on("/gen_204", HTTP_GET, captiveProbeHandler);
  server.on("/hotspot-detect.html", HTTP_GET, captiveProbeHandler);
  server.on("/library/test/success.html", HTTP_GET, captiveProbeHandler);
  server.on("/ncsi.txt", HTTP_GET, captiveProbeHandler);
  server.on("/connecttest.txt", HTTP_GET, captiveProbeHandler);
  server.on("/fwlink", HTTP_GET, captiveProbeHandler);

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

  server.on("/api/time", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (request->getResponse() != nullptr) {
      return;
    }

    metrics_record_http_request();
    const AsyncWebParameter* param = findRequestParam(request, "value");
    if (param == nullptr) {
      sendStatusMessage(request, 400, "error", "time value is required");
      return;
    }
    handleManualTimeRequest(request, param->value());
  }, nullptr, [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    if (request->getResponse() != nullptr) {
      return;
    }

    if (index == 0) {
      metrics_record_http_request();
      if (total > 128) {
        request->_tempObject = nullptr;
        sendStatusMessage(request, 413, "error", "time payload is too large");
        return;
      }

      String* body = new String();
      body->reserve(total);
      request->_tempObject = body;
    }

    String* body = static_cast<String*>(request->_tempObject);
    if (body == nullptr) {
      sendStatusMessage(request, 500, "error", "request buffer allocation failed");
      return;
    }

    if (body->length() + len > 128) {
      delete body;
      request->_tempObject = nullptr;
      sendStatusMessage(request, 413, "error", "time payload is too large");
      return;
    }

    body->concat(reinterpret_cast<const char*>(data), len);
    if (index + len != total) {
      return;
    }

    String value;
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, *body);
    if (!error && doc["value"].is<const char*>()) {
      value = doc["value"].as<const char*>();
    } else {
      value = *body;
    }

    delete body;
    request->_tempObject = nullptr;
    handleManualTimeRequest(request, value);
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
    const SunOfflineMode currentMode = sun_offline_mode_from_elevation(currentPosition.elevation_deg);

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

    if (!ensureSunPathCache(
            localTm.tm_year + 1900,
            localTm.tm_mon + 1,
            localTm.tm_mday,
            latitude,
            longitude,
            tzOffsetMinutes,
            midnightEpoch)) {
      sendStatusMessage(request, 500, "error", "failed to calculate sun path");
      return;
    }

    doc["pathSampleMinutes"] = kSunPathSampleStepMinutes;
    doc["pathAlgorithm"] = "raw-10m-threshold-crossings-v3";
    float pathMinElevation = 90.0f;
    float pathMaxElevation = -90.0f;

    SunPathExtraPoint extraPoints[16] {};
    size_t extraPointCount = 0;
    constexpr float kSunPathThresholds[] = { 0.0f, -6.0f, -12.0f, -18.0f };

    for (size_t index = 1; index < s_sunPathCache.count; ++index) {
      const SunPathPoint& prev = s_sunPathCache.points[index - 1];
      const SunPathPoint& curr = s_sunPathCache.points[index];

      for (float threshold : kSunPathThresholds) {
        SunPathExtraPoint crossing {};
        if (findSunPathThresholdCrossing(prev, curr, threshold, latitude, longitude, midnightEpoch, crossing)) {
          addSunPathExtraPoint(
              extraPoints,
              sizeof(extraPoints) / sizeof(extraPoints[0]),
              extraPointCount,
              crossing);
        }
      }
    }
    sortSunPathExtraPoints(extraPoints, extraPointCount);

    const auto appendPathPoint = [&](JsonArray& path, uint32_t secondOfDay, float azimuth, float elevation) {
      JsonObject point = path.add<JsonObject>();
      point["minute"] = static_cast<float>(secondOfDay) / 60.0f;
      point["azimuth"] = azimuth;
      point["elevation"] = elevation;
      point["mode"] = sun_offline_mode_name(sun_offline_mode_from_elevation(static_cast<double>(elevation)));
      if (elevation < pathMinElevation) {
        pathMinElevation = elevation;
      }
      if (elevation > pathMaxElevation) {
        pathMaxElevation = elevation;
      }
    };

    JsonArray path = doc["path"].to<JsonArray>();
    size_t extraPointIndex = 0;
    for (size_t index = 0; index < s_sunPathCache.count; ++index) {
      const SunPathPoint& cachedPoint = s_sunPathCache.points[index];
      const uint32_t sampleSecond = static_cast<uint32_t>(cachedPoint.minute) * 60UL;
      while (extraPointIndex < extraPointCount && extraPoints[extraPointIndex].secondOfDay < sampleSecond) {
        const SunPathExtraPoint& point = extraPoints[extraPointIndex];
        appendPathPoint(path, point.secondOfDay, point.azimuth, point.elevation);
        ++extraPointIndex;
      }

      if (extraPointIndex < extraPointCount && extraPoints[extraPointIndex].secondOfDay == sampleSecond) {
        const SunPathExtraPoint& point = extraPoints[extraPointIndex];
        appendPathPoint(path, point.secondOfDay, point.azimuth, point.elevation);
        ++extraPointIndex;
      } else {
        appendPathPoint(path, sampleSecond, cachedPoint.azimuth, cachedPoint.elevation);
      }
    }

    while (extraPointIndex < extraPointCount) {
      const SunPathExtraPoint& point = extraPoints[extraPointIndex];
      appendPathPoint(path, point.secondOfDay, point.azimuth, point.elevation);
      ++extraPointIndex;
    }
    doc["pathMinElevation"] = pathMinElevation;
    doc["pathMaxElevation"] = pathMaxElevation;
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
    moon_led_apply();

    const Config cfg = getConfig();
    populateMoonLedDoc(doc["moonLed"].to<JsonObject>(), cfg);

    sendJsonDocument(request, 200, doc, true);
  });

  server.on("/api/moon-led", HTTP_POST, [](AsyncWebServerRequest* request) {
    metrics_record_http_request();

    Config cfg = getConfig();

    bool enabled = cfg.moonLed.enabled;
    if (parseBooleanValue(request, "enabled", enabled)) {
      cfg.moonLed.enabled = enabled;
    }

    uint8_t maxBrightness = cfg.moonLed.maxBrightness;
    if (parseBrightnessValue(request, maxBrightness)) {
      cfg.moonLed.maxBrightness = maxBrightness;
    }

    const AsyncWebParameter* hueParam = findRequestParam(request, "hue");
    if (hueParam != nullptr) {
      const int parsedHue = hueParam->value().toInt();
      if (parsedHue < 0 || parsedHue > 360) {
        sendStatusMessage(request, 400, "error", "hue must be an integer from 0 to 360");
        return;
      }
      cfg.moonLed.hue = static_cast<uint16_t>(parsedHue);
    }

    if (!configUpdate(cfg, true)) {
      sendStatusMessage(request, 500, "error", "failed to save moon led settings");
      return;
    }

    moon_led_apply();

    JsonDocument doc;
    doc["status"] = "ok";
    populateMoonLedDoc(doc["moonLed"].to<JsonObject>(), cfg);
    sendJsonDocument(request, 200, doc, true);
  });

  server.on("/api/save", HTTP_POST, [](AsyncWebServerRequest *request) {
  }, nullptr, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    if (request->getResponse() != nullptr) {
      return;
    }

    if (index == 0) {
      metrics_record_http_request();
      if (total > kSaveJsonMaxBytes) {
        request->_tempObject = nullptr;
        sendStatusMessage(request, 413, "error", "settings payload is too large");
        return;
      }

      String* body = new String();
      body->reserve(total);
      request->_tempObject = body;
    }

    String* body = static_cast<String*>(request->_tempObject);
    if (body == nullptr) {
      sendStatusMessage(request, 500, "error", "request buffer allocation failed");
      return;
    }

    if (body->length() + len > kSaveJsonMaxBytes) {
      delete body;
      request->_tempObject = nullptr;
      sendStatusMessage(request, 413, "error", "settings payload is too large");
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

    Config cfg = getConfig();
    const JsonObject root = doc.as<JsonObject>();

    copyJsonString(cfg.devname, root["devname"]);
    copyJsonString(cfg.name, root["name"]);

    if (root["ssid"].is<const char*>()) {
      WiFiCredentials storedCredentials {};
      (void)wifi_get_credentials(storedCredentials);

      const char* ssid = root["ssid"] | "";
      const char* password = storedCredentials.password;
      if (root["password"].is<const char*>()) {
        const char* requestedPassword = root["password"] | "";
        if (requestedPassword[0] != '\0' || strcmp(ssid, storedCredentials.ssid) != 0) {
          password = requestedPassword;
        }
      }

      if (!wifi_save_credentials(ssid, password)) {
        sendStatusMessage(request, 500, "error", "failed to save Wi-Fi credentials");
        return;
      }
    }

    copyJsonString(cfg.ntp.ntp_server, root["ntp_server"]);
    copyJsonString(cfg.ntp.ntp_server2, root["ntp_server2"]);
    copyJsonString(cfg.ntp.ntp_timezone, root["ntp_timezone"]);

    double latitude = cfg.location.lat;
    if (parseJsonCoordinate(root["lat"], latitude) && latitude >= -90.0 && latitude <= 90.0) {
      cfg.location.lat = latitude;
    }

    double longitude = cfg.location.lng;
    if (parseJsonCoordinate(root["lon"], longitude) && longitude >= -180.0 && longitude <= 180.0) {
      cfg.location.lng = longitude;
    }

    const bool hasValidTime = ntp_has_valid_time();

    bool autoMode = cfg.location.enabled;
    if (parseJsonBool(root["autoMode"], autoMode)) {
      cfg.location.enabled = autoMode;
    }

    bool autoTimeMode = cfg.timeSchedule.enabled;
    if (parseJsonBool(root["autoTimeMode"], autoTimeMode)) {
      cfg.timeSchedule.enabled = autoTimeMode;
    }

    normalizeAutoModes(cfg);

    parseJsonMinuteOfDay(root["timeOnMinute"], cfg.timeSchedule.onMinute);
    parseJsonMinuteOfDay(root["timeOffMinute"], cfg.timeSchedule.offMinute);

    uint8_t brightness = cfg.brightness;
    if (parseJsonBrightness(root["brightness"], brightness)) {
      cfg.brightness = brightness;
    }

    if (hasManualCandleControl(cfg, hasValidTime)) {
      bool candleOn = cfg.candleOn;
      if (parseJsonBool(root["candleOn"], candleOn)) {
        cfg.candleOn = candleOn;
      }
      if (cfg.brightness == 0) {
        cfg.candleOn = false;
      }
    }

    applyMoonLedJson(cfg, root["moonLed"].as<JsonObjectConst>());

    if (!configUpdate(cfg, true)) {
      sendStatusMessage(request, 500, "error", "failed to save settings");
      return;
    }

    i2c_set_brightness(resolveCandleBrightness(cfg, hasValidTime, sun_position_should_enable_now()));
    moon_led_apply();
    JsonDocument docResponse;
    docResponse["status"] = "ok";
    docResponse["restarting"] = true;
    docResponse["message"] = "settings saved, device restarting";
    sendJsonDocument(request, 200, docResponse);
    scheduleDeviceRestart(750, "settings");
  });

  server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest *request){
    metrics_record_http_request();
    wifi_clear_credentials();

    if (!configSave()) {
      sendStatusMessage(request, 500, "error", "failed to clear Wi-Fi credentials");
      return;
    }

    i2c_set_brightness(getConfig().brightness);
    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Wi-Fi credentials cleared, device restarting";
    sendJsonDocument(request, 200, doc);
    scheduleDeviceRestart(750, "wifi-reset");
  });

  const auto rawUpdateHandler = [](bool defaultFilesystemUpdate) {
    return [defaultFilesystemUpdate](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      UpdateRequestState* updateState = ensureUpdateRequestState(request);
      if (updateState == nullptr) {
        return;
      }

      if (index == 0) {
        metrics_record_http_request();
        String filename = request->header("X-Update-Filename");
        filename.trim();
        beginUpdateRequest(request, filename, defaultFilesystemUpdate, total, true);
      }

      writeUpdateChunk(updateState, index, data, len);

      if (total != 0 && index + len >= total) {
        String filename = request->header("X-Update-Filename");
        filename.trim();
        finishUpdateRequest(updateState, filename, index + len);
      }
    };
  };

  server.on(
      "/api/update/firmware",
      HTTP_POST,
      [](AsyncWebServerRequest* request) {
        metrics_record_http_request();
        sendUpdateResult(request);
      },
      nullptr,
      rawUpdateHandler(false));

  server.on(
      "/api/update/filesystem",
      HTTP_POST,
      [](AsyncWebServerRequest* request) {
        metrics_record_http_request();
        sendUpdateResult(request);
      },
      nullptr,
      rawUpdateHandler(true));

  server.on("/api/update/restart", HTTP_POST, [](AsyncWebServerRequest* request) {
    metrics_record_http_request();
    JsonDocument doc;
    doc["status"] = "ok";
    doc["restarting"] = true;
    const bool scheduled = scheduleDeviceRestart(1500, "ota-update");
    doc["restartScheduled"] = scheduled;
    doc["restartInMs"] = restartInMs();
    doc["message"] = "device restarting";
    sendJsonDocument(request, 200, doc, true);
  });

  server.on("/api/update", HTTP_GET, [](AsyncWebServerRequest* request) {
    metrics_record_http_request();
    const UpdateStateSnapshot updateSnapshot = s_updateStore.snapshot();
    JsonDocument doc;
    doc["status"] = "ok";
    doc["active"] = updateSnapshot.active;
    doc["error"] = updateSnapshot.failed ? updateSnapshot.error : "";
    doc["target"] = updateSnapshot.filesystemTarget ? "filesystem" : "firmware";
    doc["maxSize"] = updateSnapshot.maxSize;
    doc["restartPending"] = s_restartScheduled;
    doc["restartInMs"] = restartInMs();
    sendJsonDocument(request, 200, doc, true);
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

    Config cfg = getConfig();
    const bool hasValidTime = ntp_has_valid_time();

    bool autoMode = cfg.location.enabled;
    if (hasValidTime && parseBooleanValue(request, "autoMode", autoMode)) {
      cfg.location.enabled = autoMode;
    }

    bool autoTimeMode = cfg.timeSchedule.enabled;
    if (hasValidTime && parseBooleanValue(request, "autoTimeMode", autoTimeMode)) {
      cfg.timeSchedule.enabled = autoTimeMode;
    }

    normalizeAutoModes(cfg);

    parseMinuteValue(request, "timeOnMinute", cfg.timeSchedule.onMinute);
    parseMinuteValue(request, "timeOffMinute", cfg.timeSchedule.offMinute);

    cfg.brightness = value;

    if (hasManualCandleControl(cfg, hasValidTime)) {
      bool candleOn = cfg.candleOn;
      if (parseBooleanValue(request, "candleOn", candleOn)) {
        cfg.candleOn = candleOn;
      }
      if (value == 0) {
        cfg.candleOn = false;
      }
    }

    if (!configUpdate(cfg, true)) {
      sendStatusMessage(request, 500, "error", "failed to save brightness");
      return;
    }

    const bool autoCandleOn = sun_position_should_enable_now();
    i2c_set_brightness(resolveCandleBrightness(cfg, hasValidTime, autoCandleOn));

    JsonDocument doc;
    doc["status"] = "ok";
    doc["brightness"] = value;
    doc["candleOn"] = cfg.candleOn;
    doc["autoMode"] = cfg.location.enabled;
    doc["autoTimeMode"] = cfg.timeSchedule.enabled;
    doc["autoCandleOn"] = autoCandleOn;
    doc["autoTimeCandleOn"] = sun_position_time_schedule_should_enable_now();
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
      redirectToCaptivePortal(request);
      return;
    }

    request->send(404, "text/plain", "Not found");
  });

  server.serveStatic("/", LittleFS, "/")
      .setCacheControl("no-store, max-age=0");

  server.begin();
  Serial.println("[web] server started");
}

void web_server_handle() {
  if (!s_restartScheduled || s_restartAtMs == 0) {
    return;
  }

  if (static_cast<int32_t>(millis() - s_restartAtMs) < 0) {
    return;
  }

  Serial.printf("[web] restarting device, reason=%s\n",
                s_restartReason[0] != '\0' ? s_restartReason : "requested");
  delay(50);
  esp_restart();
}
