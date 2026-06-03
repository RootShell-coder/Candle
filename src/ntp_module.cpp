#include "ntp_module.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "config.h"
#include "metrics.h"

namespace {
constexpr uint32_t kNtpSyncIntervalMs = 6UL * 60UL * 60UL * 1000UL;
constexpr uint32_t kNtpSyncTimeoutMs = 60000UL;
constexpr uint32_t kNtpRetryBaseMs = 15000UL;
constexpr uint32_t kNtpRetryMaxMs = 15UL * 60UL * 1000UL;
constexpr time_t kValidEpochThreshold = static_cast<time_t>(1704067200ULL);

bool s_initialized = false;
bool s_syncInProgress = false;
bool s_bootSyncPending = true;
bool s_lastWifiConnected = false;
uint8_t s_failureStreak = 0;
uint32_t s_nextSyncAtMs = 0;
uint32_t s_syncDeadlineMs = 0;
time_t s_lastRequestedEpoch = 0;
char s_lastServer[64] = "";
char s_lastTimezone[32] = "";
char s_posixTimezone[32] = "UTC0";

// Проверяет, что полученное Unix-время уже похоже на реальное,
// а не осталось в стартовом/нулевом состоянии после загрузки устройства.
bool ntp_time_is_valid(time_t value) {
  return value >= kValidEpochThreshold;
}

// Вычисляет задержку до следующей попытки синхронизации,
// постепенно увеличивая интервал после последовательных ошибок.
uint32_t ntp_retry_delay_ms() {
  uint32_t delayMs = kNtpRetryBaseMs;

  for (uint8_t attempt = 1; attempt < s_failureStreak && delayMs < kNtpRetryMaxMs; ++attempt) {
    if (delayMs > (kNtpRetryMaxMs / 2U)) {
      delayMs = kNtpRetryMaxMs;
      break;
    }
    delayMs *= 2U;
  }

  return delayMs;
}

// Пытается разобрать таймзону, заданную как числовое смещение часов
// относительно UTC, например `+3`, `-5` или `UTC+2`.
bool ntp_parse_timezone_hours(const char* raw, long& hours) {
  if (raw == nullptr) {
    return false;
  }

  while (*raw == ' ' || *raw == '\t') {
    ++raw;
  }

  if (strncasecmp(raw, "UTC", 3) == 0 || strncasecmp(raw, "GMT", 3) == 0) {
    raw += 3;
  }

  if (*raw == '\0') {
    return false;
  }

  char* end = nullptr;
  hours = strtol(raw, &end, 10);
  while (*end == ' ' || *end == '\t') {
    ++end;
  }

  return end != raw && *end == '\0' && hours >= -12 && hours <= 14;
}

// Преобразует значение таймзоны из конфигурации в POSIX-строку,
// которую понимают `configTzTime()` и стандартные функции времени ESP32.
const char* ntp_resolve_timezone(const char* raw) {
  if (raw == nullptr || *raw == '\0') {
    strlcpy(s_posixTimezone, "UTC0", sizeof(s_posixTimezone));
    return s_posixTimezone;
  }

  if (strcasecmp(raw, "Europe/London") == 0) {
    strlcpy(s_posixTimezone, "GMT0BST,M3.5.0/1,M10.5.0/2", sizeof(s_posixTimezone));
    return s_posixTimezone;
  }

  if (strcasecmp(raw, "Europe/Moscow") == 0) {
    strlcpy(s_posixTimezone, "MSK-3", sizeof(s_posixTimezone));
    return s_posixTimezone;
  }

  if (strcasecmp(raw, "UTC") == 0 || strcasecmp(raw, "Etc/UTC") == 0 || strcasecmp(raw, "GMT") == 0) {
    strlcpy(s_posixTimezone, "UTC0", sizeof(s_posixTimezone));
    return s_posixTimezone;
  }

  long hours = 0;
  if (ntp_parse_timezone_hours(raw, hours)) {
    snprintf(s_posixTimezone, sizeof(s_posixTimezone), "UTC%+ld", -hours);
    return s_posixTimezone;
  }

  if (strchr(raw, ',') != nullptr) {
    strlcpy(s_posixTimezone, raw, sizeof(s_posixTimezone));
    return s_posixTimezone;
  }

  Serial.printf("[ntp] unsupported timezone '%s', falling back to UTC0\n", raw);
  strlcpy(s_posixTimezone, "UTC0", sizeof(s_posixTimezone));
  return s_posixTimezone;
}

// Callback от SNTP, который вызывается после успешной синхронизации времени.
// Здесь обновляются метрики, сбрасываются ошибки и планируется следующий sync.
void ntp_on_time_sync(struct timeval* tv) {
  const time_t syncedEpoch = tv != nullptr ? tv->tv_sec : time(nullptr);
  int32_t driftSeconds = 0;

  if (ntp_time_is_valid(s_lastRequestedEpoch) && ntp_time_is_valid(syncedEpoch)) {
    const int64_t drift = static_cast<int64_t>(syncedEpoch) - static_cast<int64_t>(s_lastRequestedEpoch);
    if (drift < INT32_MIN) {
      driftSeconds = INT32_MIN;
    } else if (drift > INT32_MAX) {
      driftSeconds = INT32_MAX;
    } else {
      driftSeconds = static_cast<int32_t>(drift);
    }
  }

  metrics_record_ntp_sync(driftSeconds, static_cast<uint32_t>(syncedEpoch));
  s_syncInProgress = false;
  s_syncDeadlineMs = 0;
  s_failureStreak = 0;
  s_nextSyncAtMs = millis() + kNtpSyncIntervalMs;

  Serial.printf(
      "[ntp] synchronized via '%s', epoch=%lld, drift=%ld s\n",
      s_lastServer[0] != '\0' ? s_lastServer : "pool.ntp.org",
      static_cast<long long>(syncedEpoch),
      static_cast<long>(driftSeconds));
}

// Проверяет, изменились ли NTP-сервер или таймзона в текущей конфигурации,
// чтобы при необходимости немедленно перезапросить синхронизацию.
bool ntp_config_changed(const Config& cfg) {
  const char* ntpServer = strlen(cfg.ntp.ntp_server) != 0 ? cfg.ntp.ntp_server : "pool.ntp.org";
  return strcmp(s_lastServer, ntpServer) != 0 ||
         strcmp(s_lastTimezone, cfg.ntp.ntp_timezone) != 0;
}

// Запускает новую попытку синхронизации через сервер из конфигурации,
// подготавливая SNTP, дедлайн ожидания и диагностические поля состояния.
void ntp_request_sync(const Config& cfg) {
  if (!s_initialized) {
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    sntp_set_time_sync_notification_cb(ntp_on_time_sync);
    s_initialized = true;
  }

  const char* ntpServer = strlen(cfg.ntp.ntp_server) != 0 ? cfg.ntp.ntp_server : "pool.ntp.org";
  const char* posixTimezone = ntp_resolve_timezone(cfg.ntp.ntp_timezone);

  s_lastRequestedEpoch = time(nullptr);
  s_syncInProgress = true;
  s_bootSyncPending = false;

  const uint32_t nowMs = millis();
  s_syncDeadlineMs = nowMs + kNtpSyncTimeoutMs;

  strlcpy(s_lastServer, ntpServer, sizeof(s_lastServer));
  strlcpy(s_lastTimezone, cfg.ntp.ntp_timezone, sizeof(s_lastTimezone));

  configTzTime(posixTimezone, ntpServer);

  Serial.printf(
      "[ntp] sync requested, server='%s', timezone='%s' -> '%s'\n",
      ntpServer,
      cfg.ntp.ntp_timezone,
      posixTimezone);
}
} // namespace

// Сбрасывает внутреннее состояние NTP-модуля при старте устройства
// или перед запуском сетевой FreeRTOS-задачи.
void ntp_init() {
  s_initialized = false;
  s_syncInProgress = false;
  s_bootSyncPending = true;
  s_lastWifiConnected = false;
  s_failureStreak = 0;
  s_nextSyncAtMs = 0;
  s_syncDeadlineMs = 0;
  s_lastRequestedEpoch = 0;
  s_lastServer[0] = '\0';
  s_lastTimezone[0] = '\0';

  // Применяем таймзону из конфига немедленно, чтобы localtime_r() и
  // ntp_utc_offset_minutes() работали корректно ещё до первой NTP-синхронизации.
  // Критично для расчёта расписания рассвета/заката при старте, когда RTC
  // содержит валидное время, но WiFi ещё не подключён.
  const char* posixTz = ntp_resolve_timezone(getConfig().ntp.ntp_timezone);
  setenv("TZ", posixTz, 1);
  tzset();
  Serial.printf("[ntp] init, timezone '%s' -> '%s'\n",
                getConfig().ntp.ntp_timezone, posixTz);
}

// Основной обработчик NTP-модуля: отслеживает состояние Wi‑Fi,
// контролирует таймауты и решает, когда нужно запустить очередной sync.
void ntp_handle() {
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;

  if (s_syncInProgress) {
    const uint32_t nowMs = millis();
    if (!wifiConnected) {
      s_syncInProgress = false;
      s_syncDeadlineMs = 0;
      s_nextSyncAtMs = 0;
      Serial.println("[ntp] Wi-Fi lost during sync, will retry after reconnect");
    } else if (static_cast<int32_t>(nowMs - s_syncDeadlineMs) >= 0) {
      metrics_record_ntp_failure();
      s_syncInProgress = false;
      s_syncDeadlineMs = 0;
      if (s_failureStreak < UINT8_MAX) {
        ++s_failureStreak;
      }
      const uint32_t retryDelayMs = ntp_retry_delay_ms();
      s_nextSyncAtMs = nowMs + retryDelayMs;
      Serial.printf("[ntp] sync timeout, retry in %lu ms (streak=%u)\n",
                    static_cast<unsigned long>(retryDelayMs),
                    static_cast<unsigned>(s_failureStreak));
    }
  }

  if (!wifiConnected) {
    s_lastWifiConnected = false;
    return;
  }

  if (!s_lastWifiConnected) {
    s_lastWifiConnected = true;
    s_bootSyncPending = true;
    s_nextSyncAtMs = 0;
    Serial.println("[ntp] Wi-Fi connected, scheduling NTP sync");
  }

  const Config& cfg = getConfig();
  const bool timeInvalid = !ntp_has_valid_time();
  const bool scheduleDue = s_nextSyncAtMs == 0 ||
                           static_cast<int32_t>(millis() - s_nextSyncAtMs) >= 0;
  const bool shouldSync = !s_syncInProgress &&
                          (s_bootSyncPending ||
                           scheduleDue ||
                           (timeInvalid && s_failureStreak == 0));

  if (ntp_config_changed(cfg)) {
    ntp_request_sync(cfg);
    return;
  }

  if (shouldSync) {
    ntp_request_sync(cfg);
  }
}

// Возвращает признак того, что текущее системное время уже прошло
// базовую проверку на валидность и может использоваться приложением.
bool ntp_has_valid_time() {
  return ntp_time_is_valid(time(nullptr));
}

// Возвращает смещение локального времени от UTC в минутах для текущего момента.
// Использует TZ env-переменную, выставленную NTP-модулем из конфига.
// Учитывает DST: значение корректно для текущего момента (летнее/зимнее время).
int ntp_utc_offset_minutes() {
  const time_t now = time(nullptr);
  struct tm utcTm {};
  if (gmtime_r(&now, &utcTm) == nullptr) {
    return 0;
  }
  // mktime интерпретирует структуру как local-время и возвращает UTC-эпоху.
  // Разница между исходным UTC и «переинтерпретированным» равна смещению TZ.
  const time_t interpretedAsLocal = mktime(&utcTm);
  if (interpretedAsLocal == static_cast<time_t>(-1)) {
    return 0;
  }
  return static_cast<int>(difftime(now, interpretedAsLocal) / 60.0);
}
