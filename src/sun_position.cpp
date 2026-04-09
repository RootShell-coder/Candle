#include "sun_position.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ctype.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "config.h"
#include "i2c.h"
#include "metrics.h"
#include "ntp_module.h"

namespace {
#define SUN_POSITION_MAX_RETRIES 5
#define SUN_BRIGHTNESS_CIVIL_REDUCTION_PERCENT 10
#define SUN_BRIGHTNESS_NAUTICAL_REDUCTION_PERCENT 20
#define SUN_BRIGHTNESS_ASTRONOMICAL_REDUCTION_PERCENT 30

constexpr char kSunCachePath[] = "/sun_position.json";
constexpr char kSunApiBaseUrl[] = "https://api.sunrise-sunset.org/json";
constexpr uint32_t kSunRequestTimeoutMs = 15000UL;
constexpr uint32_t kSunRetryIntervalMs = 60UL * 60UL * 1000UL;
constexpr uint32_t kSunWorkerIdleMs = 1000UL;
constexpr uint32_t kSunWorkerYieldMs = 50UL;
constexpr uint32_t kSunTaskStack = 6144;
constexpr UBaseType_t kSunTaskPriority = 1;
constexpr BaseType_t kSunTaskCore = 0;
constexpr uint16_t kSunRefreshMinuteOfDay = 10; // 00:10 local time
constexpr uint8_t kSunModeUnknown = 0;
constexpr uint8_t kSunModeDay = 1;
constexpr uint8_t kSunModeCivil = 2;
constexpr uint8_t kSunModeNautical = 3;
constexpr uint8_t kSunModeAstronomical = 4;
constexpr uint8_t kSunModeNight = 5;
constexpr uint8_t kSunModeDisabled = 6;

bool s_initialized = false;
bool s_hasCache = false;
bool s_outputKnown = false;
bool s_candleEnabled = false;
bool s_lastWifiConnected = false;
bool s_wifiRefreshPending = false;
volatile bool s_fetchRequested = false;
volatile bool s_fetchInProgress = false;
TaskHandle_t s_sunTaskHandle = nullptr;
uint8_t s_lastAppliedBrightness = 0;
uint8_t s_attemptsToday = 0;
uint16_t s_sunriseMinutes = 6U * 60U;
uint16_t s_sunsetMinutes = 18U * 60U;
uint16_t s_civilTwilightBeginMinutes = 6U * 60U;
uint16_t s_civilTwilightEndMinutes = 18U * 60U;
uint16_t s_nauticalTwilightBeginMinutes = 6U * 60U;
uint16_t s_nauticalTwilightEndMinutes = 18U * 60U;
uint16_t s_astronomicalTwilightBeginMinutes = 6U * 60U;
uint16_t s_astronomicalTwilightEndMinutes = 18U * 60U;
uint32_t s_nextAttemptAtMs = 0;
char s_cachedDate[11] = "";
char s_attemptDay[11] = "";
char s_requestedDate[11] = "";

// Разбирает две цифры подряд и возвращает их как целое число.
bool parse_two_digits(const char* text, int& value) {
  if (text == nullptr ||
      !isdigit(static_cast<unsigned char>(text[0])) ||
      !isdigit(static_cast<unsigned char>(text[1]))) {
    return false;
  }

  value = (text[0] - '0') * 10 + (text[1] - '0');
  return true;
}

// Разбирает четыре цифры подряд и возвращает их как целое число.
bool parse_four_digits(const char* text, int& value) {
  if (text == nullptr ||
      !isdigit(static_cast<unsigned char>(text[0])) ||
      !isdigit(static_cast<unsigned char>(text[1])) ||
      !isdigit(static_cast<unsigned char>(text[2])) ||
      !isdigit(static_cast<unsigned char>(text[3]))) {
    return false;
  }

  value = (text[0] - '0') * 1000 +
          (text[1] - '0') * 100 +
          (text[2] - '0') * 10 +
          (text[3] - '0');
  return true;
}

// Возвращает число дней с 1970-01-01 для указанной календарной даты.
int64_t days_from_civil(int year, unsigned month, unsigned day) {
  year -= month <= 2 ? 1 : 0;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const int adjustedMonth = static_cast<int>(month) + (month > 2 ? -3 : 9);
  const unsigned doy = (153U * static_cast<unsigned>(adjustedMonth) + 2U) / 5U + day - 1U;
  const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
  return static_cast<int64_t>(era) * 146097LL + static_cast<int64_t>(doe) - 719468LL;
}

// Преобразует ISO-8601 время из API (обычно UTC) в минуты от локальной полуночи.
bool parse_minutes_from_iso8601(const char* raw, uint16_t& minutes) {
  if (raw == nullptr) {
    return false;
  }

  const size_t len = strlen(raw);
  if (len < 17 || raw[4] != '-' || raw[7] != '-' || raw[10] != 'T' || raw[13] != ':') {
    return false;
  }

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;

  if (!parse_four_digits(raw, year) ||
      !parse_two_digits(raw + 5, month) ||
      !parse_two_digits(raw + 8, day) ||
      !parse_two_digits(raw + 11, hour) ||
      !parse_two_digits(raw + 14, minute)) {
    return false;
  }

  size_t timezonePos = 16;
  if (len > 16 && raw[16] == ':') {
    if (!parse_two_digits(raw + 17, second)) {
      return false;
    }
    timezonePos = 19;
  }

  int apiOffsetMinutes = 0;
  if (len <= timezonePos) {
    return false;
  }

  if (raw[timezonePos] == 'Z') {
    apiOffsetMinutes = 0;
  } else if ((raw[timezonePos] == '+' || raw[timezonePos] == '-') &&
             len >= timezonePos + 6 &&
             raw[timezonePos + 3] == ':') {
    int offsetHour = 0;
    int offsetMinute = 0;
    if (!parse_two_digits(raw + timezonePos + 1, offsetHour) ||
        !parse_two_digits(raw + timezonePos + 4, offsetMinute)) {
      return false;
    }

    apiOffsetMinutes = offsetHour * 60 + offsetMinute;
    if (raw[timezonePos] == '-') {
      apiOffsetMinutes = -apiOffsetMinutes;
    }
  } else {
    return false;
  }

  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
    return false;
  }

  const int64_t daysSinceEpoch = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  const int64_t secondsSinceEpoch =
      daysSinceEpoch * 86400LL +
      static_cast<int64_t>(hour) * 3600LL +
      static_cast<int64_t>(minute) * 60LL +
      static_cast<int64_t>(second) -
      static_cast<int64_t>(apiOffsetMinutes) * 60LL;

  const time_t utcEpoch = static_cast<time_t>(secondsSinceEpoch);
  struct tm localTm {};
  if (localtime_r(&utcEpoch, &localTm) == nullptr) {
    return false;
  }

  minutes = static_cast<uint16_t>(localTm.tm_hour * 60 + localTm.tm_min);
  return true;
}

// Преобразует AM/PM-представление времени в минуты от полуночи.
bool parse_minutes_from_ampm(const char* raw, uint16_t& minutes) {
  if (raw == nullptr || *raw == '\0') {
    return false;
  }

  char buffer[24];
  strlcpy(buffer, raw, sizeof(buffer));

  char* suffix = strrchr(buffer, ' ');
  if (suffix == nullptr) {
    return false;
  }

  *suffix = '\0';
  ++suffix;

  const bool isPm = strcasecmp(suffix, "PM") == 0;
  const bool isAm = strcasecmp(suffix, "AM") == 0;
  if (!isPm && !isAm) {
    return false;
  }

  char* end = nullptr;
  long hour = strtol(buffer, &end, 10);
  if (end == buffer || *end != ':' || hour < 1 || hour > 12) {
    return false;
  }

  long minute = strtol(end + 1, &end, 10);
  if (minute < 0 || minute > 59) {
    return false;
  }

  if (*end == ':') {
    strtol(end + 1, &end, 10);
  }

  if (*end != '\0') {
    return false;
  }

  if (isPm && hour < 12) {
    hour += 12;
  }
  if (isAm && hour == 12) {
    hour = 0;
  }

  minutes = static_cast<uint16_t>(hour * 60 + minute);
  return true;
}

// Пытается разобрать время API в любом из поддерживаемых форматов.
bool parse_api_minutes(const char* raw, uint16_t& minutes) {
  return parse_minutes_from_iso8601(raw, minutes) || parse_minutes_from_ampm(raw, minutes);
}

// Кодирует строку для безопасной передачи в URL-параметре.
String url_encode(const char* raw) {
  static const char kHex[] = "0123456789ABCDEF";
  String encoded;

  if (raw == nullptr) {
    return encoded;
  }

  for (const unsigned char* ptr = reinterpret_cast<const unsigned char*>(raw); *ptr != '\0'; ++ptr) {
    if (isalnum(*ptr) || *ptr == '-' || *ptr == '_' || *ptr == '.' || *ptr == '~') {
      encoded += static_cast<char>(*ptr);
    } else {
      encoded += '%';
      encoded += kHex[*ptr >> 4];
      encoded += kHex[*ptr & 0x0F];
    }
  }

  return encoded;
}

// Формирует локальную дату `YYYY-MM-DD` и текущую минуту суток.
bool build_local_date_key(char* dateKey, size_t dateKeySize, uint16_t& minuteOfDay) {
  if (!ntp_has_valid_time()) {
    return false;
  }

  time_t now = time(nullptr);
  struct tm localTm {};
  localtime_r(&now, &localTm);

  minuteOfDay = static_cast<uint16_t>(localTm.tm_hour * 60 + localTm.tm_min);
  const unsigned year = static_cast<unsigned>(localTm.tm_year + 1900) % 10000U;
  const unsigned month = static_cast<unsigned>(localTm.tm_mon + 1);
  const unsigned day = static_cast<unsigned>(localTm.tm_mday);

  snprintf(
      dateKey,
      dateKeySize,
      "%04u-%02u-%02u",
      year,
      month,
      day);

  return true;
}

// Загружает расписание солнца из LittleFS и обновляет runtime-кэш.
bool load_cached_schedule() {
  File file = LittleFS.open(kSunCachePath, "r");
  if (!file) {
    s_hasCache = false;
    Serial.println("[sun] cache file not found");
    metrics_set_sun_state(false, s_candleEnabled, s_fetchInProgress, s_lastAppliedBrightness, s_attemptsToday);
    return false;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    s_hasCache = false;
    Serial.printf("[sun] failed to parse cache: %s\n", error.c_str());
    metrics_set_sun_state(false, s_candleEnabled, s_fetchInProgress, s_lastAppliedBrightness, s_attemptsToday);
    return false;
  }

  uint16_t sunriseMinutes = doc["sunrise_minutes"] | 0;
  uint16_t sunsetMinutes = doc["sunset_minutes"] | 0;

  if ((sunriseMinutes >= (24 * 60) || sunsetMinutes >= (24 * 60) || sunriseMinutes == sunsetMinutes) &&
      doc["sunrise"].is<const char*>()) {
    if (!parse_api_minutes(doc["sunrise"], sunriseMinutes)) {
      s_hasCache = false;
      metrics_set_sun_state(false, s_candleEnabled, s_fetchInProgress, s_lastAppliedBrightness, s_attemptsToday);
      return false;
    }
  }

  if ((sunriseMinutes >= (24 * 60) || sunsetMinutes >= (24 * 60) || sunriseMinutes == sunsetMinutes) &&
      doc["sunset"].is<const char*>()) {
    if (!parse_api_minutes(doc["sunset"], sunsetMinutes)) {
      s_hasCache = false;
      metrics_set_sun_state(false, s_candleEnabled, s_fetchInProgress, s_lastAppliedBrightness, s_attemptsToday);
      return false;
    }
  }

  if (sunriseMinutes >= (24 * 60) || sunsetMinutes >= (24 * 60) || sunriseMinutes == sunsetMinutes) {
    s_hasCache = false;
    Serial.println("[sun] cached schedule is invalid");
    metrics_set_sun_state(false, s_candleEnabled, s_fetchInProgress, s_lastAppliedBrightness, s_attemptsToday);
    return false;
  }

  uint16_t civilBeginMinutes = doc["civil_twilight_begin_minutes"] | sunriseMinutes;
  uint16_t civilEndMinutes = doc["civil_twilight_end_minutes"] | sunsetMinutes;
  uint16_t nauticalBeginMinutes = doc["nautical_twilight_begin_minutes"] | civilBeginMinutes;
  uint16_t nauticalEndMinutes = doc["nautical_twilight_end_minutes"] | civilEndMinutes;
  uint16_t astronomicalBeginMinutes = doc["astronomical_twilight_begin_minutes"] | nauticalBeginMinutes;
  uint16_t astronomicalEndMinutes = doc["astronomical_twilight_end_minutes"] | nauticalEndMinutes;
  const uint32_t updatedAtEpoch = doc["updated_at"] | 0U;

  if (doc["civil_twilight_begin"].is<const char*>()) {
    parse_api_minutes(doc["civil_twilight_begin"], civilBeginMinutes);
  }
  if (doc["civil_twilight_end"].is<const char*>()) {
    parse_api_minutes(doc["civil_twilight_end"], civilEndMinutes);
  }
  if (doc["nautical_twilight_begin"].is<const char*>()) {
    parse_api_minutes(doc["nautical_twilight_begin"], nauticalBeginMinutes);
  }
  if (doc["nautical_twilight_end"].is<const char*>()) {
    parse_api_minutes(doc["nautical_twilight_end"], nauticalEndMinutes);
  }
  if (doc["astronomical_twilight_begin"].is<const char*>()) {
    parse_api_minutes(doc["astronomical_twilight_begin"], astronomicalBeginMinutes);
  }
  if (doc["astronomical_twilight_end"].is<const char*>()) {
    parse_api_minutes(doc["astronomical_twilight_end"], astronomicalEndMinutes);
  }

  s_sunriseMinutes = sunriseMinutes;
  s_sunsetMinutes = sunsetMinutes;
  s_civilTwilightBeginMinutes = civilBeginMinutes;
  s_civilTwilightEndMinutes = civilEndMinutes;
  s_nauticalTwilightBeginMinutes = nauticalBeginMinutes;
  s_nauticalTwilightEndMinutes = nauticalEndMinutes;
  s_astronomicalTwilightBeginMinutes = astronomicalBeginMinutes;
  s_astronomicalTwilightEndMinutes = astronomicalEndMinutes;
  strlcpy(s_cachedDate, doc["date"] | "", sizeof(s_cachedDate));
  s_hasCache = true;

  metrics_set_sun_schedule(
      updatedAtEpoch,
      s_sunriseMinutes,
      s_sunsetMinutes,
      s_civilTwilightBeginMinutes,
      s_civilTwilightEndMinutes,
      s_nauticalTwilightBeginMinutes,
      s_nauticalTwilightEndMinutes,
      s_astronomicalTwilightBeginMinutes,
      s_astronomicalTwilightEndMinutes);
  metrics_set_sun_state(true, s_candleEnabled, s_fetchInProgress, s_lastAppliedBrightness, s_attemptsToday);

  Serial.printf(
      "[sun] cache loaded: date=%s sunrise=%02u:%02u sunset=%02u:%02u civil=%02u:%02u-%02u:%02u nautical=%02u:%02u-%02u:%02u astro=%02u:%02u-%02u:%02u\n",
      s_cachedDate[0] != '\0' ? s_cachedDate : "n/a",
      static_cast<unsigned>(s_sunriseMinutes / 60U),
      static_cast<unsigned>(s_sunriseMinutes % 60U),
      static_cast<unsigned>(s_sunsetMinutes / 60U),
      static_cast<unsigned>(s_sunsetMinutes % 60U),
      static_cast<unsigned>(s_civilTwilightBeginMinutes / 60U),
      static_cast<unsigned>(s_civilTwilightBeginMinutes % 60U),
      static_cast<unsigned>(s_civilTwilightEndMinutes / 60U),
      static_cast<unsigned>(s_civilTwilightEndMinutes % 60U),
      static_cast<unsigned>(s_nauticalTwilightBeginMinutes / 60U),
      static_cast<unsigned>(s_nauticalTwilightBeginMinutes % 60U),
      static_cast<unsigned>(s_nauticalTwilightEndMinutes / 60U),
      static_cast<unsigned>(s_nauticalTwilightEndMinutes % 60U),
      static_cast<unsigned>(s_astronomicalTwilightBeginMinutes / 60U),
      static_cast<unsigned>(s_astronomicalTwilightBeginMinutes % 60U),
      static_cast<unsigned>(s_astronomicalTwilightEndMinutes / 60U),
      static_cast<unsigned>(s_astronomicalTwilightEndMinutes % 60U));

  return true;
}

// Сохраняет свежее расписание солнца в локальный кэш LittleFS.
bool save_cached_schedule(
    const char* dateKey,
    const char* sunriseRaw,
    const char* sunsetRaw,
    const char* civilBeginRaw,
    const char* civilEndRaw,
    const char* nauticalBeginRaw,
    const char* nauticalEndRaw,
    const char* astronomicalBeginRaw,
    const char* astronomicalEndRaw,
    uint16_t sunriseMinutes,
    uint16_t sunsetMinutes,
    uint16_t civilBeginMinutes,
    uint16_t civilEndMinutes,
    uint16_t nauticalBeginMinutes,
    uint16_t nauticalEndMinutes,
    uint16_t astronomicalBeginMinutes,
    uint16_t astronomicalEndMinutes) {
  File file = LittleFS.open(kSunCachePath, "w");
  if (!file) {
    Serial.println("[sun] failed to open cache file for writing");
    return false;
  }

  JsonDocument doc;
  doc["date"] = dateKey;
  doc["sunrise"] = sunriseRaw;
  doc["sunset"] = sunsetRaw;
  doc["civil_twilight_begin"] = civilBeginRaw;
  doc["civil_twilight_end"] = civilEndRaw;
  doc["nautical_twilight_begin"] = nauticalBeginRaw;
  doc["nautical_twilight_end"] = nauticalEndRaw;
  doc["astronomical_twilight_begin"] = astronomicalBeginRaw;
  doc["astronomical_twilight_end"] = astronomicalEndRaw;
  doc["sunrise_minutes"] = sunriseMinutes;
  doc["sunset_minutes"] = sunsetMinutes;
  doc["civil_twilight_begin_minutes"] = civilBeginMinutes;
  doc["civil_twilight_end_minutes"] = civilEndMinutes;
  doc["nautical_twilight_begin_minutes"] = nauticalBeginMinutes;
  doc["nautical_twilight_end_minutes"] = nauticalEndMinutes;
  doc["astronomical_twilight_begin_minutes"] = astronomicalBeginMinutes;
  doc["astronomical_twilight_end_minutes"] = astronomicalEndMinutes;
  doc["updated_at"] = static_cast<uint32_t>(time(nullptr));

  const bool ok = serializeJsonPretty(doc, file) > 0;
  file.close();

  if (!ok) {
    Serial.println("[sun] failed to write cache JSON");
  }

  return ok;
}

// Запрашивает новое расписание солнца у внешнего API и перезагружает кэш.
bool fetch_schedule_from_api(const Config& cfg, const char* dateKey) {
  String url = String(kSunApiBaseUrl) +
               "?lat=" + String(cfg.location.lat, 6) +
               "&lng=" + String(cfg.location.lng, 6) +
               "&date=" + String(dateKey) +
               "&formatted=0";

  if (cfg.ntp.ntp_timezone[0] != '\0') {
    url += "&tzid=" + url_encode(cfg.ntp.ntp_timezone);
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(kSunRequestTimeoutMs);

  if (!http.begin(client, url)) {
    Serial.println("[sun] failed to start HTTPS request");
    return false;
  }

  http.addHeader("Accept", "application/json");
  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[sun] HTTP request failed: %d\n", httpCode);
    http.end();
    return false;
  }

  const String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.printf("[sun] failed to parse API response: %s\n", error.c_str());
    return false;
  }

  const char* status = doc["status"] | "";
  if (strcmp(status, "OK") != 0 && strcmp(status, "INVALID_TZID") != 0) {
    Serial.printf("[sun] API returned status '%s'\n", status);
    return false;
  }

  const char* sunriseRaw = doc["results"]["sunrise"] | "";
  const char* sunsetRaw = doc["results"]["sunset"] | "";
  const char* civilBeginRaw = doc["results"]["civil_twilight_begin"] | "";
  const char* civilEndRaw = doc["results"]["civil_twilight_end"] | "";
  const char* nauticalBeginRaw = doc["results"]["nautical_twilight_begin"] | "";
  const char* nauticalEndRaw = doc["results"]["nautical_twilight_end"] | "";
  const char* astronomicalBeginRaw = doc["results"]["astronomical_twilight_begin"] | "";
  const char* astronomicalEndRaw = doc["results"]["astronomical_twilight_end"] | "";
  uint16_t sunriseMinutes = 0;
  uint16_t sunsetMinutes = 0;
  uint16_t civilBeginMinutes = 0;
  uint16_t civilEndMinutes = 0;
  uint16_t nauticalBeginMinutes = 0;
  uint16_t nauticalEndMinutes = 0;
  uint16_t astronomicalBeginMinutes = 0;
  uint16_t astronomicalEndMinutes = 0;

  if (!parse_api_minutes(sunriseRaw, sunriseMinutes) ||
      !parse_api_minutes(sunsetRaw, sunsetMinutes) ||
      !parse_api_minutes(civilBeginRaw, civilBeginMinutes) ||
      !parse_api_minutes(civilEndRaw, civilEndMinutes) ||
      !parse_api_minutes(nauticalBeginRaw, nauticalBeginMinutes) ||
      !parse_api_minutes(nauticalEndRaw, nauticalEndMinutes) ||
      !parse_api_minutes(astronomicalBeginRaw, astronomicalBeginMinutes) ||
      !parse_api_minutes(astronomicalEndRaw, astronomicalEndMinutes) ||
      sunriseMinutes == sunsetMinutes) {
    Serial.println("[sun] sunrise/sunset values are invalid");
    return false;
  }

  if (!save_cached_schedule(
          dateKey,
          sunriseRaw,
          sunsetRaw,
          civilBeginRaw,
          civilEndRaw,
          nauticalBeginRaw,
          nauticalEndRaw,
          astronomicalBeginRaw,
          astronomicalEndRaw,
          sunriseMinutes,
          sunsetMinutes,
          civilBeginMinutes,
          civilEndMinutes,
          nauticalBeginMinutes,
          nauticalEndMinutes,
          astronomicalBeginMinutes,
          astronomicalEndMinutes)) {
    return false;
  }

  if (!load_cached_schedule()) {
    Serial.println("[sun] saved cache could not be reloaded");
    return false;
  }

  metrics_record_sun_update_success();

  Serial.printf(
      "[sun] API updated and reloaded from %s: sunrise=%02u:%02u sunset=%02u:%02u civil=%02u:%02u-%02u:%02u nautical=%02u:%02u-%02u:%02u astro=%02u:%02u-%02u:%02u status=%s\n",
      kSunCachePath,
      static_cast<unsigned>(s_sunriseMinutes / 60U),
      static_cast<unsigned>(s_sunriseMinutes % 60U),
      static_cast<unsigned>(s_sunsetMinutes / 60U),
      static_cast<unsigned>(s_sunsetMinutes % 60U),
      static_cast<unsigned>(s_civilTwilightBeginMinutes / 60U),
      static_cast<unsigned>(s_civilTwilightBeginMinutes % 60U),
      static_cast<unsigned>(s_civilTwilightEndMinutes / 60U),
      static_cast<unsigned>(s_civilTwilightEndMinutes % 60U),
      static_cast<unsigned>(s_nauticalTwilightBeginMinutes / 60U),
      static_cast<unsigned>(s_nauticalTwilightBeginMinutes % 60U),
      static_cast<unsigned>(s_nauticalTwilightEndMinutes / 60U),
      static_cast<unsigned>(s_nauticalTwilightEndMinutes % 60U),
      static_cast<unsigned>(s_astronomicalTwilightBeginMinutes / 60U),
      static_cast<unsigned>(s_astronomicalTwilightBeginMinutes % 60U),
      static_cast<unsigned>(s_astronomicalTwilightEndMinutes / 60U),
      static_cast<unsigned>(s_astronomicalTwilightEndMinutes % 60U),
      status);

  return true;
}

// Уменьшает яркость на заданный процент без выхода за диапазон.
uint8_t apply_percent_reduction(uint8_t value, uint8_t reductionPercent) {
  const uint16_t scaled = static_cast<uint16_t>(value) * static_cast<uint16_t>(100U - reductionPercent) / 100U;
  return static_cast<uint8_t>(scaled);
}

// Возвращает короткое имя текущего солнечного режима.
const char* sun_mode_name_local(uint8_t mode) {
  switch (mode) {
    case kSunModeDay:
      return "day";
    case kSunModeCivil:
      return "civil";
    case kSunModeNautical:
      return "nautical";
    case kSunModeAstronomical:
      return "astronomical";
    case kSunModeNight:
      return "night";
    case kSunModeDisabled:
      return "manual";
    case kSunModeUnknown:
    default:
      return "unknown";
  }
}

// Определяет солнечный режим по минуте локальных суток.
uint8_t resolve_sun_mode(uint16_t minuteOfDay) {
  if (minuteOfDay < s_astronomicalTwilightBeginMinutes || minuteOfDay >= s_astronomicalTwilightEndMinutes) {
    return kSunModeNight;
  }

  if (minuteOfDay < s_nauticalTwilightBeginMinutes || minuteOfDay >= s_nauticalTwilightEndMinutes) {
    return kSunModeAstronomical;
  }

  if (minuteOfDay < s_civilTwilightBeginMinutes || minuteOfDay >= s_civilTwilightEndMinutes) {
    return kSunModeNautical;
  }

  if (minuteOfDay < s_sunriseMinutes || minuteOfDay >= s_sunsetMinutes) {
    return kSunModeCivil;
  }

  return kSunModeDay;
}

// Вычисляет целевую яркость для текущего времени с учётом twilight-правил.
uint8_t resolve_brightness_for_time(uint16_t minuteOfDay) {
  const uint8_t baseBrightness = getConfig().brightness;

  switch (resolve_sun_mode(minuteOfDay)) {
    case kSunModeNight:
      return baseBrightness;
    case kSunModeAstronomical:
      return apply_percent_reduction(baseBrightness, SUN_BRIGHTNESS_ASTRONOMICAL_REDUCTION_PERCENT);
    case kSunModeNautical:
      return apply_percent_reduction(baseBrightness, SUN_BRIGHTNESS_NAUTICAL_REDUCTION_PERCENT);
    case kSunModeCivil:
      return apply_percent_reduction(baseBrightness, SUN_BRIGHTNESS_CIVIL_REDUCTION_PERCENT);
    case kSunModeDay:
    case kSunModeUnknown:
    default:
      return 0;
  }
}

// Применяет рассчитанное состояние свечи к I2C-матрице и метрикам.
void apply_candle_state(uint16_t minuteOfDay) {
  if (!s_hasCache || s_sunriseMinutes >= (24 * 60) || s_sunsetMinutes >= (24 * 60)) {
    metrics_set_sun_mode(kSunModeUnknown);
    metrics_set_sun_state(false, false, s_fetchInProgress, 0, s_attemptsToday);
    return;
  }

  const uint8_t currentMode = resolve_sun_mode(minuteOfDay);
  const uint8_t targetBrightness = resolve_brightness_for_time(minuteOfDay);
  const bool shouldEnable = targetBrightness > 0;

  if (!s_outputKnown ||
      s_candleEnabled != shouldEnable ||
      s_lastAppliedBrightness != targetBrightness) {
    i2c_set_brightness(targetBrightness);
    s_outputKnown = true;
    s_candleEnabled = shouldEnable;
    s_lastAppliedBrightness = targetBrightness;

    Serial.printf(
        "[sun] candle %s at %02u:%02u (sunrise=%02u:%02u sunset=%02u:%02u brightness=%u civil-%u%% nautical-%u%% astro-%u%%)\n",
        shouldEnable ? "ON" : "OFF",
        static_cast<unsigned>(minuteOfDay / 60U),
        static_cast<unsigned>(minuteOfDay % 60U),
        static_cast<unsigned>(s_sunriseMinutes / 60U),
        static_cast<unsigned>(s_sunriseMinutes % 60U),
        static_cast<unsigned>(s_sunsetMinutes / 60U),
        static_cast<unsigned>(s_sunsetMinutes % 60U),
        static_cast<unsigned>(targetBrightness),
        static_cast<unsigned>(SUN_BRIGHTNESS_CIVIL_REDUCTION_PERCENT),
        static_cast<unsigned>(SUN_BRIGHTNESS_NAUTICAL_REDUCTION_PERCENT),
        static_cast<unsigned>(SUN_BRIGHTNESS_ASTRONOMICAL_REDUCTION_PERCENT));
  }

  metrics_set_sun_mode(currentMode);
  metrics_set_sun_state(true, shouldEnable, s_fetchInProgress, targetBrightness, s_attemptsToday);
}

// Ставит флаг фонового обновления расписания для указанной даты.
void schedule_refresh_request(const char* dateKey) {
  strlcpy(s_requestedDate, dateKey, sizeof(s_requestedDate));
  s_fetchRequested = true;
  metrics_record_sun_refresh_request();
  metrics_set_sun_state(s_hasCache, s_candleEnabled, s_fetchInProgress, s_lastAppliedBrightness, s_attemptsToday);
  Serial.printf("[sun] async refresh scheduled for %s\n", s_requestedDate);
}

// Обрабатывает неудачное обновление и планирует следующую попытку.
void handle_fetch_failure() {
  metrics_record_sun_update_failure();

  if (s_attemptsToday < UINT8_MAX) {
    ++s_attemptsToday;
  }

  metrics_set_sun_state(s_hasCache, s_candleEnabled, s_fetchInProgress, s_lastAppliedBrightness, s_attemptsToday);

  if (s_attemptsToday < SUN_POSITION_MAX_RETRIES) {
    s_nextAttemptAtMs = millis() + kSunRetryIntervalMs;
    Serial.printf(
        "[sun] update failed, retry %u/%u in %lu ms\n",
        static_cast<unsigned>(s_attemptsToday),
        static_cast<unsigned>(SUN_POSITION_MAX_RETRIES),
        static_cast<unsigned long>(kSunRetryIntervalMs));
  } else {
    s_nextAttemptAtMs = 0;
    Serial.printf(
        "[sun] update failed, retry budget exhausted (%u/%u), using cached schedule\n",
        static_cast<unsigned>(s_attemptsToday),
        static_cast<unsigned>(SUN_POSITION_MAX_RETRIES));
  }
}

// Сбрасывает лимиты ретраев после успешного обновления расписания.
void handle_fetch_success() {
  s_attemptsToday = 0;
  s_nextAttemptAtMs = 0;

  uint16_t minuteOfDay = 0;
  char currentDate[11] = "";
  if (build_local_date_key(currentDate, sizeof(currentDate), minuteOfDay)) {
    apply_candle_state(minuteOfDay);
  }
}

// Возвращает управление яркостью в ручной режим, если авто-режим отключён.
void apply_manual_brightness_when_sun_control_disabled() {
  const Config& cfg = getConfig();
  const bool shouldEnable = cfg.candleOn && cfg.brightness > 0;
  const uint8_t targetBrightness = shouldEnable ? cfg.brightness : 0;

  s_fetchRequested = false;
  metrics_set_sun_control_mode(false);
  metrics_set_sun_mode(kSunModeDisabled);

  if (!s_outputKnown ||
      s_candleEnabled != shouldEnable ||
      s_lastAppliedBrightness != targetBrightness) {
    i2c_set_brightness(targetBrightness);
    s_outputKnown = true;
    s_candleEnabled = shouldEnable;
    s_lastAppliedBrightness = targetBrightness;

    Serial.printf(
        "[sun] control disabled in config, using manual brightness=%u\n",
        static_cast<unsigned>(targetBrightness));
  }

  metrics_set_sun_state(s_hasCache, shouldEnable, s_fetchInProgress, targetBrightness, s_attemptsToday);
}

// Выполняет один шаг логики автояркости и решения о refresh расписания.
void process_sun_logic() {
  if (!getConfig().location.enabled) {
    apply_manual_brightness_when_sun_control_disabled();
    s_lastWifiConnected = (WiFi.status() == WL_CONNECTED);
    s_wifiRefreshPending = false;
    return;
  }

  metrics_set_sun_control_mode(true);

  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  if (wifiConnected && !s_lastWifiConnected) {
    s_wifiRefreshPending = true;
    s_nextAttemptAtMs = 0;
    Serial.println("[sun] Wi-Fi connected, scheduling immediate sun refresh");
  }
  s_lastWifiConnected = wifiConnected;

  if (!ntp_has_valid_time()) {
    return;
  }

  char todayKey[11] = "";
  uint16_t minuteOfDay = 0;
  if (!build_local_date_key(todayKey, sizeof(todayKey), minuteOfDay)) {
    return;
  }

  if (strcmp(s_attemptDay, todayKey) != 0) {
    strlcpy(s_attemptDay, todayKey, sizeof(s_attemptDay));
    s_attemptsToday = 0;
    s_nextAttemptAtMs = 0;
  }

  apply_candle_state(minuteOfDay);

  const bool hasTodayData = s_hasCache && strcmp(s_cachedDate, todayKey) == 0;
  const bool shouldRefresh = s_wifiRefreshPending || !s_hasCache || (!hasTodayData && minuteOfDay >= kSunRefreshMinuteOfDay);
  if (!shouldRefresh) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (s_attemptsToday >= SUN_POSITION_MAX_RETRIES) {
    return;
  }

  if (s_nextAttemptAtMs != 0 && static_cast<int32_t>(millis() - s_nextAttemptAtMs) < 0) {
    return;
  }

  if (s_fetchRequested || s_fetchInProgress) {
    return;
  }

  s_wifiRefreshPending = false;
  schedule_refresh_request(todayKey);
}

// Фоновая FreeRTOS-задача обслуживает автояркость и загрузку расписания.
void sun_position_worker_task(void* pvParameters) {
  (void)pvParameters;
  char dateKey[11] = "";

  for (;;) {
    process_sun_logic();

    if (!s_fetchRequested || s_fetchInProgress) {
      vTaskDelay(pdMS_TO_TICKS(kSunWorkerIdleMs));
      continue;
    }

    if (WiFi.status() != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(kSunWorkerIdleMs));
      continue;
    }

    strlcpy(dateKey, s_requestedDate, sizeof(dateKey));
    s_fetchRequested = false;
    s_fetchInProgress = true;
    metrics_set_sun_state(s_hasCache, s_candleEnabled, s_fetchInProgress, s_lastAppliedBrightness, s_attemptsToday);

    if (fetch_schedule_from_api(getConfig(), dateKey)) {
      handle_fetch_success();
    } else {
      handle_fetch_failure();
    }

    s_fetchInProgress = false;
    metrics_set_sun_state(s_hasCache, s_candleEnabled, s_fetchInProgress, s_lastAppliedBrightness, s_attemptsToday);
    vTaskDelay(pdMS_TO_TICKS(kSunWorkerYieldMs));
  }
}
}  // namespace

// Инициализирует модуль солнца, восстанавливает кэш и запускает worker-задачу.
void sun_position_init() {
  s_initialized = true;
  s_hasCache = false;
  s_outputKnown = false;
  s_candleEnabled = false;
  s_lastWifiConnected = false;
  s_wifiRefreshPending = false;
  s_lastAppliedBrightness = 0;
  s_attemptsToday = 0;
  s_sunriseMinutes = 6U * 60U;
  s_sunsetMinutes = 18U * 60U;
  s_civilTwilightBeginMinutes = 6U * 60U;
  s_civilTwilightEndMinutes = 18U * 60U;
  s_nauticalTwilightBeginMinutes = 6U * 60U;
  s_nauticalTwilightEndMinutes = 18U * 60U;
  s_astronomicalTwilightBeginMinutes = 6U * 60U;
  s_astronomicalTwilightEndMinutes = 18U * 60U;
  s_nextAttemptAtMs = 0;
  s_cachedDate[0] = '\0';
  s_attemptDay[0] = '\0';
  s_requestedDate[0] = '\0';
  s_fetchRequested = false;
  s_fetchInProgress = false;
  metrics_set_sun_control_mode(getConfig().location.enabled);
  metrics_set_sun_mode(kSunModeUnknown);
  metrics_set_sun_state(false, false, false, 0, 0);

  load_cached_schedule();

  if (!getConfig().location.enabled) {
    apply_manual_brightness_when_sun_control_disabled();
  } else {
    uint16_t minuteOfDay = 0;
    char todayKey[11] = "";

    if (s_hasCache && ntp_has_valid_time() && build_local_date_key(todayKey, sizeof(todayKey), minuteOfDay)) {
      apply_candle_state(minuteOfDay);
      const bool shouldEnable = resolve_brightness_for_time(minuteOfDay) > 0;
      const char* modeName = sun_mode_name_local(resolve_sun_mode(minuteOfDay));
      Serial.printf(
          "[sun] startup auto mode applied: candle=%s, mode=%s\n",
          shouldEnable ? "ON" : "OFF",
          modeName);
    } else {
      i2c_set_brightness(0);
      s_outputKnown = true;
      s_candleEnabled = false;
      s_lastAppliedBrightness = 0;
      metrics_set_sun_state(s_hasCache, false, s_fetchInProgress, 0, s_attemptsToday);
      Serial.println("[sun] startup auto mode waiting for valid time/schedule");
    }
  }

  if (s_sunTaskHandle == nullptr) {
    const BaseType_t created = xTaskCreatePinnedToCore(
        sun_position_worker_task,
        "SunPositionTask",
        kSunTaskStack,
        nullptr,
        kSunTaskPriority,
        &s_sunTaskHandle,
        kSunTaskCore);
    if (created != pdPASS) {
      Serial.println("[sun] failed to start worker task");
      s_sunTaskHandle = nullptr;
    }
  }
}

// Возвращает текущее фактическое состояние выхода свечи.
bool sun_position_is_candle_enabled() {
  if (!getConfig().location.enabled) {
    return getConfig().candleOn && getConfig().brightness > 0;
  }

  return s_outputKnown ? s_candleEnabled : false;
}

// Показывает, должна ли свеча быть включена по текущему расписанию.
bool sun_position_should_enable_now() {
  if (!s_hasCache || !ntp_has_valid_time()) {
    return false;
  }

  uint16_t minuteOfDay = 0;
  char todayKey[11] = "";
  if (!build_local_date_key(todayKey, sizeof(todayKey), minuteOfDay)) {
    return false;
  }

  return resolve_brightness_for_time(minuteOfDay) > 0;
}

// Возвращает имя текущего режима автояркости для web/API.
const char* sun_position_current_mode_name() {
  if (!getConfig().location.enabled) {
    return "manual";
  }

  if (!s_hasCache || !ntp_has_valid_time()) {
    return "unknown";
  }

  uint16_t minuteOfDay = 0;
  char todayKey[11] = "";
  if (!build_local_date_key(todayKey, sizeof(todayKey), minuteOfDay)) {
    return "unknown";
  }

  return sun_mode_name_local(resolve_sun_mode(minuteOfDay));
}

// Лениво инициализирует модуль при первом обращении из основного кода.
void sun_position_handle() {
  if (!s_initialized) {
    sun_position_init();
  }
}
