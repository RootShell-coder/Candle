#include "ntp_module.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

#include "config.h"
#include "metrics.h"

namespace {
constexpr uint32_t kNtpSyncIntervalMs = 6UL * 60UL * 60UL * 1000UL;
constexpr uint32_t kNtpSyncTimeoutMs = 60000UL;
constexpr uint32_t kNtpRetryBaseMs = 15000UL;
constexpr uint32_t kNtpRetryMaxMs = 15UL * 60UL * 1000UL;
constexpr time_t kValidEpochThreshold = static_cast<time_t>(1704067200ULL);
constexpr const char* kDefaultNtpServer = "pool.ntp.org";

bool s_initialized = false;
bool s_syncInProgress = false;
bool s_bootSyncPending = true;
bool s_lastWifiConnected = false;
uint8_t s_failureStreak = 0;
uint32_t s_nextSyncAtMs = 0;
uint32_t s_syncDeadlineMs = 0;
time_t s_lastRequestedEpoch = 0;
uint32_t s_lastSuccessEpoch = 0;
char s_lastServer[64] = "";
char s_lastServer2[64] = "";
char s_lastServerIp[46] = "";
char s_lastServer2Ip[46] = "";
char s_lastTimezone[32] = "";
char s_posixTimezone[32] = "UTC0";
bool s_lastDnsResolved = false;

bool ntp_time_is_valid(time_t value) {
  return value >= kValidEpochThreshold;
}

bool ntp_has_sta_ip() {
  return WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

bool ntp_resolve_server(const char* hostname, char* resolved, size_t resolvedSize) {
  if (resolved == nullptr || resolvedSize == 0) {
    return false;
  }

  resolved[0] = '\0';
  if (hostname == nullptr || hostname[0] == '\0') {
    return false;
  }

  IPAddress ip;
  if (!WiFi.hostByName(hostname, ip)) {
    return false;
  }

  const String ipText = ip.toString();
  if (ipText.length() == 0 || ipText == "0.0.0.0") {
    return false;
  }

  strlcpy(resolved, ipText.c_str(), resolvedSize);
  return true;
}

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

void ntp_on_time_sync(struct timeval* tv) {
  const time_t syncedEpoch = tv != nullptr ? tv->tv_sec : time(nullptr);
  if (!s_syncInProgress && ntp_time_is_valid(s_lastSuccessEpoch)) {
    const int64_t secondsSinceLastSuccess =
        static_cast<int64_t>(syncedEpoch) - static_cast<int64_t>(s_lastSuccessEpoch);
    if (secondsSinceLastSuccess >= -2 && secondsSinceLastSuccess <= 2) {
      return;
    }
  }

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
  s_lastSuccessEpoch = static_cast<uint32_t>(syncedEpoch);

  Serial.printf(
      "[ntp] synchronized via '%s', epoch=%lld, drift=%ld s\n",
      s_lastServer[0] != '\0' ? s_lastServer : "pool.ntp.org",
      static_cast<long long>(syncedEpoch),
      static_cast<long>(driftSeconds));
}

bool ntp_config_changed(const Config& cfg) {
  const char* ntpServer = strlen(cfg.ntp.ntp_server) != 0 ? cfg.ntp.ntp_server : kDefaultNtpServer;
  const char* ntpServer2 = strlen(cfg.ntp.ntp_server2) != 0 ? cfg.ntp.ntp_server2 : "";
  return strcmp(s_lastServer, ntpServer) != 0 ||
         strcmp(s_lastServer2, ntpServer2) != 0 ||
         strcmp(s_lastTimezone, cfg.ntp.ntp_timezone) != 0;
}

void ntp_request_sync(const Config& cfg) {
  if (!s_initialized) {
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    sntp_set_time_sync_notification_cb(ntp_on_time_sync);
    s_initialized = true;
  }

  const char* ntpServer = strlen(cfg.ntp.ntp_server) != 0 ? cfg.ntp.ntp_server : kDefaultNtpServer;
  const char* posixTimezone = ntp_resolve_timezone(cfg.ntp.ntp_timezone);

  s_lastRequestedEpoch = time(nullptr);
  s_syncInProgress = true;
  s_bootSyncPending = false;

  const uint32_t nowMs = millis();
  s_syncDeadlineMs = nowMs + kNtpSyncTimeoutMs;

  strlcpy(s_lastServer, ntpServer, sizeof(s_lastServer));
  strlcpy(s_lastServer2, cfg.ntp.ntp_server2, sizeof(s_lastServer2));
  strlcpy(s_lastTimezone, cfg.ntp.ntp_timezone, sizeof(s_lastTimezone));

  const char* ntpServer2 = strlen(cfg.ntp.ntp_server2) != 0 ? cfg.ntp.ntp_server2 : nullptr;
  const bool primaryDnsOk = ntp_resolve_server(ntpServer, s_lastServerIp, sizeof(s_lastServerIp));
  bool secondaryDnsOk = true;
  s_lastServer2Ip[0] = '\0';
  if (ntpServer2 != nullptr) {
    secondaryDnsOk = ntp_resolve_server(ntpServer2, s_lastServer2Ip, sizeof(s_lastServer2Ip));
  }
  s_lastDnsResolved = primaryDnsOk;

  if (!primaryDnsOk) {
    metrics_record_ntp_failure();
    s_syncInProgress = false;
    s_syncDeadlineMs = 0;
    if (s_failureStreak < UINT8_MAX) {
      ++s_failureStreak;
    }
    const uint32_t retryDelayMs = ntp_retry_delay_ms();
    s_nextSyncAtMs = nowMs + retryDelayMs;
    Serial.printf("[ntp] DNS resolve failed for '%s', retry in %lu ms (streak=%u)\n",
                  ntpServer,
                  static_cast<unsigned long>(retryDelayMs),
                  static_cast<unsigned>(s_failureStreak));
    return;
  }

  if (ntpServer2 != nullptr && !secondaryDnsOk) {
    Serial.printf("[ntp] DNS resolve failed for secondary server '%s', using primary only\n", ntpServer2);
  }

  configTzTime(
      posixTimezone,
      s_lastServerIp,
      ntpServer2 != nullptr && secondaryDnsOk ? s_lastServer2Ip : nullptr);
  sntp_set_time_sync_notification_cb(ntp_on_time_sync);

  Serial.printf(
      "[ntp] sync requested, server='%s' (%s), timezone='%s' -> '%s'\n",
      ntpServer,
      s_lastServerIp,
      cfg.ntp.ntp_timezone,
      posixTimezone);
}
} // namespace

void ntp_init() {
  s_initialized = false;
  s_syncInProgress = false;
  s_bootSyncPending = true;
  s_lastWifiConnected = false;
  s_failureStreak = 0;
  s_nextSyncAtMs = 0;
  s_syncDeadlineMs = 0;
  s_lastRequestedEpoch = 0;
  s_lastSuccessEpoch = 0;
  s_lastServer[0] = '\0';
  s_lastServer2[0] = '\0';
  s_lastServerIp[0] = '\0';
  s_lastServer2Ip[0] = '\0';
  s_lastTimezone[0] = '\0';
  s_lastDnsResolved = false;

  const char* posixTz = ntp_resolve_timezone(getConfig().ntp.ntp_timezone);
  setenv("TZ", posixTz, 1);
  tzset();
  Serial.printf("[ntp] init, timezone '%s' -> '%s'\n",
                getConfig().ntp.ntp_timezone, posixTz);
}

void ntp_handle() {
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  const bool hasIp = wifiConnected && ntp_has_sta_ip();

  if (s_syncInProgress) {
    const uint32_t nowMs = millis();
    const sntp_sync_status_t syncStatus = sntp_get_sync_status();
    if (syncStatus == SNTP_SYNC_STATUS_COMPLETED) {
      ntp_on_time_sync(nullptr);
    } else if (!wifiConnected || !hasIp) {
      s_syncInProgress = false;
      s_syncDeadlineMs = 0;
      s_nextSyncAtMs = 0;
      Serial.println("[ntp] Wi-Fi/IP lost during sync, will retry after reconnect");
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

  if (!wifiConnected || !hasIp) {
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

bool ntp_has_valid_time() {
  return ntp_time_is_valid(time(nullptr));
}

NtpStatus ntp_status() {
  const uint32_t nowMs = millis();
  NtpStatus status {};
  status.wifiConnected = WiFi.status() == WL_CONNECTED;
  status.hasIp = status.wifiConnected && ntp_has_sta_ip();
  status.syncInProgress = s_syncInProgress;
  status.validTime = ntp_has_valid_time();
  status.ntpSynchronized = ntp_is_synchronized();
  status.bootSyncPending = s_bootSyncPending;
  status.dnsResolved = s_lastDnsResolved;
  status.failureStreak = s_failureStreak;
  status.sntpSyncStatus = static_cast<uint8_t>(sntp_get_sync_status());
  status.lastSuccessEpoch = s_lastSuccessEpoch;

  if (s_nextSyncAtMs != 0 && static_cast<int32_t>(s_nextSyncAtMs - nowMs) > 0) {
    status.nextSyncInMs = s_nextSyncAtMs - nowMs;
  }

  if (s_syncInProgress && s_syncDeadlineMs != 0 && static_cast<int32_t>(s_syncDeadlineMs - nowMs) > 0) {
    status.syncTimeoutInMs = s_syncDeadlineMs - nowMs;
  }

  strlcpy(status.server, s_lastServer[0] != '\0' ? s_lastServer : kDefaultNtpServer, sizeof(status.server));
  strlcpy(status.serverIp, s_lastServerIp, sizeof(status.serverIp));
  strlcpy(status.timezone, s_lastTimezone[0] != '\0' ? s_lastTimezone : getConfig().ntp.ntp_timezone, sizeof(status.timezone));
  return status;
}

int ntp_utc_offset_minutes() {
  const time_t now = time(nullptr);
  struct tm utcTm {};
  if (gmtime_r(&now, &utcTm) == nullptr) {
    return 0;
  }
  const time_t interpretedAsLocal = mktime(&utcTm);
  if (interpretedAsLocal == static_cast<time_t>(-1)) {
    return 0;
  }
  return static_cast<int>(difftime(now, interpretedAsLocal) / 60.0);
}

bool ntp_is_synchronized() {
  return ntp_time_is_valid(static_cast<time_t>(s_lastSuccessEpoch));
}

bool ntp_set_manual_time(uint32_t epochUtc) {
  if (ntp_is_synchronized()) {
    return false;
  }

  if (!ntp_time_is_valid(static_cast<time_t>(epochUtc))) {
    return false;
  }

  struct timeval tv {};
  tv.tv_sec = static_cast<time_t>(epochUtc);
  tv.tv_usec = 0;
  if (settimeofday(&tv, nullptr) != 0) {
    return false;
  }

  s_syncInProgress = false;
  s_syncDeadlineMs = 0;
  s_bootSyncPending = false;
  Serial.printf("[ntp] manual time set, epoch=%lu\n", static_cast<unsigned long>(epochUtc));
  return true;
}
