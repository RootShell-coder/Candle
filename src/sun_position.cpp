#include "sun_position.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "i2c.h"
#include "metrics.h"
#include "ntp_module.h"
#include "sun_offline.h"

namespace {
#define SUN_BRIGHTNESS_CIVIL_REDUCTION_PERCENT 10
#define SUN_BRIGHTNESS_NAUTICAL_REDUCTION_PERCENT 20
#define SUN_BRIGHTNESS_ASTRONOMICAL_REDUCTION_PERCENT 30

constexpr uint16_t kMinutesPerDay = 24U * 60U;
constexpr uint32_t kSunWorkerIdleMs = 1000UL;
constexpr uint32_t kSunWorkerYieldMs = 50UL;
constexpr uint32_t kSunTaskStack = 6144;
constexpr UBaseType_t kSunTaskPriority = 1;
constexpr BaseType_t kSunTaskCore = 0;

constexpr uint8_t kSunModeUnknown = 0;
constexpr uint8_t kSunModeDay = 1;
constexpr uint8_t kSunModeCivil = 2;
constexpr uint8_t kSunModeNautical = 3;
constexpr uint8_t kSunModeAstronomical = 4;
constexpr uint8_t kSunModeNight = 5;
constexpr uint8_t kSunModeDisabled = 6;

bool s_initialized = false;
bool s_hasSchedule = false;
bool s_outputKnown = false;
bool s_candleEnabled = false;
TaskHandle_t s_sunTaskHandle = nullptr;
uint8_t s_lastAppliedBrightness = 0;
uint8_t s_lastModeMetric = kSunModeUnknown;
char s_scheduleDate[11] = "";
SunOfflineEvents s_events {};

uint8_t apply_percent_reduction(uint8_t value, uint8_t reductionPercent) {
  const uint16_t scaled = static_cast<uint16_t>(value) * static_cast<uint16_t>(100U - reductionPercent) / 100U;
  return static_cast<uint8_t>(scaled);
}

uint8_t metric_mode_from_offline_mode(SunOfflineMode mode) {
  switch (mode) {
    case SunOfflineMode::Day:
      return kSunModeDay;
    case SunOfflineMode::Civil:
      return kSunModeCivil;
    case SunOfflineMode::Nautical:
      return kSunModeNautical;
    case SunOfflineMode::Astronomical:
      return kSunModeAstronomical;
    case SunOfflineMode::Night:
    default:
      return kSunModeNight;
  }
}

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

bool build_local_date_key(char* dateKey, size_t dateKeySize, uint16_t& minuteOfDay, struct tm* outLocalTm = nullptr) {
  if (!ntp_has_valid_time()) {
    return false;
  }

  const time_t now = time(nullptr);
  struct tm localTm {};
  if (localtime_r(&now, &localTm) == nullptr) {
    return false;
  }

  minuteOfDay = static_cast<uint16_t>(localTm.tm_hour * 60 + localTm.tm_min);
  const unsigned year = static_cast<unsigned>(localTm.tm_year + 1900) % 10000U;
  const unsigned month = static_cast<unsigned>(localTm.tm_mon + 1);
  const unsigned day = static_cast<unsigned>(localTm.tm_mday);

  snprintf(dateKey, dateKeySize, "%04u-%02u-%02u", year, month, day);

  if (outLocalTm != nullptr) {
    *outLocalTm = localTm;
  }

  return true;
}

uint8_t resolve_brightness_for_mode(SunOfflineMode mode) {
  const uint8_t baseBrightness = getConfig().brightness;

  switch (mode) {
    case SunOfflineMode::Night:
      return baseBrightness;
    case SunOfflineMode::Astronomical:
      return apply_percent_reduction(baseBrightness, SUN_BRIGHTNESS_ASTRONOMICAL_REDUCTION_PERCENT);
    case SunOfflineMode::Nautical:
      return apply_percent_reduction(baseBrightness, SUN_BRIGHTNESS_NAUTICAL_REDUCTION_PERCENT);
    case SunOfflineMode::Civil:
      return apply_percent_reduction(baseBrightness, SUN_BRIGHTNESS_CIVIL_REDUCTION_PERCENT);
    case SunOfflineMode::Day:
    default:
      return 0;
  }
}

bool current_mode_from_elevation(SunOfflineMode& mode) {
  if (!ntp_has_valid_time()) {
    return false;
  }

  const Config& cfg = getConfig();
  SunOfflinePosition position {};
  if (!sun_offline_calculate_position_utc(time(nullptr), cfg.location.lat, cfg.location.lng, position)) {
    return false;
  }

  mode = sun_offline_mode_from_elevation(position.elevation_deg);
  return true;
}

void publish_schedule_metrics(time_t nowUtc) {
  uint32_t updatedAtEpoch = 0;
  if (nowUtc > 0) {
    updatedAtEpoch = static_cast<uint32_t>(nowUtc);
  }

  const uint16_t sunrise = s_events.has_sunrise ? s_events.sunrise_minute : 0;
  const uint16_t sunset = s_events.has_sunset ? s_events.sunset_minute : 0;

  const uint16_t civilBegin = s_events.has_civil_twilight ? s_events.civil_begin_minute : sunrise;
  const uint16_t civilEnd = s_events.has_civil_twilight ? s_events.civil_end_minute : sunset;

  const uint16_t nauticalBegin = s_events.has_nautical_twilight ? s_events.nautical_begin_minute : civilBegin;
  const uint16_t nauticalEnd = s_events.has_nautical_twilight ? s_events.nautical_end_minute : civilEnd;

  const uint16_t astroBegin = s_events.has_astronomical_twilight ? s_events.astronomical_begin_minute : nauticalBegin;
  const uint16_t astroEnd = s_events.has_astronomical_twilight ? s_events.astronomical_end_minute : nauticalEnd;

  metrics_set_sun_schedule(
      updatedAtEpoch,
      sunrise,
      sunset,
      civilBegin,
      civilEnd,
      nauticalBegin,
      nauticalEnd,
      astroBegin,
      astroEnd);
}

bool recalc_schedule_for_today() {
  uint16_t minuteOfDay = 0;
  char todayKey[11] = "";
  struct tm localTm {};

  if (!build_local_date_key(todayKey, sizeof(todayKey), minuteOfDay, &localTm)) {
    return false;
  }

  const time_t nowUtc = time(nullptr);
  const int tzOffsetMinutes = ntp_utc_offset_minutes();

  SunOfflineEvents events {};
  const bool ok = sun_offline_calculate_events_local_day(
      localTm.tm_year + 1900,
      localTm.tm_mon + 1,
      localTm.tm_mday,
      getConfig().location.lat,
      getConfig().location.lng,
      tzOffsetMinutes,
      events);

  if (!ok) {
    s_hasSchedule = false;
    s_scheduleDate[0] = '\0';
    metrics_record_sun_update_failure();
    return false;
  }

  s_events = events;
  s_hasSchedule = true;
  strlcpy(s_scheduleDate, todayKey, sizeof(s_scheduleDate));

  publish_schedule_metrics(nowUtc);

  metrics_record_sun_update_success();
  return true;
}

bool ensure_current_day_schedule(uint16_t& minuteOfDay) {
  char todayKey[11] = "";
  if (!build_local_date_key(todayKey, sizeof(todayKey), minuteOfDay)) {
    return false;
  }

  if (!s_hasSchedule || strcmp(s_scheduleDate, todayKey) != 0) {
    metrics_record_sun_refresh_request();
    if (!recalc_schedule_for_today()) {
      return false;
    }
  }

  return true;
}

bool current_minute_of_day(uint16_t& minuteOfDay) {
  char dateKey[11] = "";
  return build_local_date_key(dateKey, sizeof(dateKey), minuteOfDay);
}

bool time_schedule_contains_minute(const TimeScheduleConfig& schedule, uint16_t minuteOfDay) {
  if (schedule.onMinute == schedule.offMinute) {
    return true;
  }
  if (schedule.onMinute < schedule.offMinute) {
    return minuteOfDay >= schedule.onMinute && minuteOfDay < schedule.offMinute;
  }
  return minuteOfDay >= schedule.onMinute || minuteOfDay < schedule.offMinute;
}

bool time_schedule_should_enable_now(const Config& cfg) {
  if (!cfg.timeSchedule.enabled || !ntp_has_valid_time()) {
    return false;
  }

  uint16_t minuteOfDay = 0;
  if (!current_minute_of_day(minuteOfDay)) {
    return false;
  }
  return time_schedule_contains_minute(cfg.timeSchedule, minuteOfDay) && cfg.brightness > 0;
}

void apply_time_schedule_brightness() {
  const Config cfg = getConfig();
  const bool shouldEnable = time_schedule_should_enable_now(cfg);
  const uint8_t targetBrightness = shouldEnable ? cfg.brightness : 0;

  metrics_set_sun_control_mode(false);
  metrics_set_sun_mode(kSunModeDisabled);

  if (!s_outputKnown ||
      s_candleEnabled != shouldEnable ||
      s_lastAppliedBrightness != targetBrightness) {
    i2c_set_brightness(targetBrightness);
    s_outputKnown = true;
    s_candleEnabled = shouldEnable;
    s_lastAppliedBrightness = targetBrightness;

    Serial.printf("[sun] time schedule brightness=%u\n", static_cast<unsigned>(targetBrightness));
  }

  metrics_set_sun_state(false, shouldEnable, false, targetBrightness, 0);
}

void apply_manual_brightness_when_sun_control_disabled() {
  const Config& cfg = getConfig();
  const bool shouldEnable = cfg.candleOn && cfg.brightness > 0;
  const uint8_t targetBrightness = shouldEnable ? cfg.brightness : 0;

  metrics_set_sun_control_mode(false);
  metrics_set_sun_mode(kSunModeDisabled);

  if (!s_outputKnown ||
      s_candleEnabled != shouldEnable ||
      s_lastAppliedBrightness != targetBrightness) {
    i2c_set_brightness(targetBrightness);
    s_outputKnown = true;
    s_candleEnabled = shouldEnable;
    s_lastAppliedBrightness = targetBrightness;

    Serial.printf("[sun] control disabled in config, using manual brightness=%u\n", static_cast<unsigned>(targetBrightness));
  }

  metrics_set_sun_state(s_hasSchedule, shouldEnable, false, targetBrightness, 0);
}

void force_off_when_schedule_unavailable() {
  metrics_set_sun_mode(kSunModeUnknown);

  if (!s_outputKnown || s_candleEnabled || s_lastAppliedBrightness != 0) {
    i2c_set_brightness(0);
    s_outputKnown = true;
    s_candleEnabled = false;
    s_lastAppliedBrightness = 0;
    metrics_record_sun_no_schedule_forced_off();
  }

  metrics_set_sun_state(false, false, false, 0, 0);
}

void apply_manual_brightness_when_time_unavailable() {
  const Config cfg = getConfig();
  const bool shouldEnable = cfg.candleOn && cfg.brightness > 0;
  const uint8_t targetBrightness = shouldEnable ? cfg.brightness : 0;

  metrics_set_sun_control_mode(true);
  metrics_set_sun_mode(kSunModeUnknown);

  if (!s_outputKnown ||
      s_candleEnabled != shouldEnable ||
      s_lastAppliedBrightness != targetBrightness) {
    i2c_set_brightness(targetBrightness);
    s_outputKnown = true;
    s_candleEnabled = shouldEnable;
    s_lastAppliedBrightness = targetBrightness;

    Serial.printf("[sun] waiting for valid time, using manual brightness=%u\n", static_cast<unsigned>(targetBrightness));
  }

  metrics_set_sun_state(false, shouldEnable, false, targetBrightness, 0);
}

void apply_candle_state(uint16_t minuteOfDay) {
  if (!s_hasSchedule) {
    force_off_when_schedule_unavailable();
    return;
  }

  SunOfflineMode mode = SunOfflineMode::Night;
  if (!current_mode_from_elevation(mode)) {
    mode = sun_offline_mode_from_events(minuteOfDay % kMinutesPerDay, s_events);
  }
  const uint8_t metricMode = metric_mode_from_offline_mode(mode);
  const uint8_t targetBrightness = resolve_brightness_for_mode(mode);
  const bool shouldEnable = targetBrightness > 0;

  if (!s_outputKnown ||
      s_candleEnabled != shouldEnable ||
      s_lastAppliedBrightness != targetBrightness) {
    i2c_set_brightness(targetBrightness);
    s_outputKnown = true;
    s_candleEnabled = shouldEnable;
    s_lastAppliedBrightness = targetBrightness;

    Serial.printf(
        "[sun] candle %s at %02u:%02u (mode=%s brightness=%u civil-%u%% nautical-%u%% astro-%u%%)\n",
        shouldEnable ? "ON" : "OFF",
        static_cast<unsigned>(minuteOfDay / 60U),
        static_cast<unsigned>(minuteOfDay % 60U),
        sun_offline_mode_name(mode),
        static_cast<unsigned>(targetBrightness),
        static_cast<unsigned>(SUN_BRIGHTNESS_CIVIL_REDUCTION_PERCENT),
        static_cast<unsigned>(SUN_BRIGHTNESS_NAUTICAL_REDUCTION_PERCENT),
        static_cast<unsigned>(SUN_BRIGHTNESS_ASTRONOMICAL_REDUCTION_PERCENT));
  }

  s_lastModeMetric = metricMode;
  metrics_set_sun_mode(metricMode);
  metrics_set_sun_state(true, shouldEnable, false, targetBrightness, 0);
}

void process_sun_logic() {
  const Config cfg = getConfig();
  if (!ntp_has_valid_time() && (cfg.location.enabled || cfg.timeSchedule.enabled)) {
    apply_manual_brightness_when_time_unavailable();
    return;
  }

  if (cfg.timeSchedule.enabled) {
    apply_time_schedule_brightness();
    return;
  }

  if (!cfg.location.enabled) {
    apply_manual_brightness_when_sun_control_disabled();
    return;
  }

  metrics_set_sun_control_mode(true);

  if (!ntp_has_valid_time()) {
    apply_manual_brightness_when_time_unavailable();
    return;
  }

  uint16_t minuteOfDay = 0;
  if (!ensure_current_day_schedule(minuteOfDay)) {
    force_off_when_schedule_unavailable();
    return;
  }

  apply_candle_state(minuteOfDay);
}

void sun_position_worker_task(void* pvParameters) {
  (void)pvParameters;

  for (;;) {
    process_sun_logic();
    vTaskDelay(pdMS_TO_TICKS(kSunWorkerIdleMs));
    vTaskDelay(pdMS_TO_TICKS(kSunWorkerYieldMs));
  }
}

}  // namespace

void sun_position_init() {
  s_initialized = true;
  s_hasSchedule = false;
  s_outputKnown = false;
  s_candleEnabled = false;
  s_lastAppliedBrightness = 0;
  s_lastModeMetric = kSunModeUnknown;
  s_scheduleDate[0] = '\0';
  memset(&s_events, 0, sizeof(s_events));

  metrics_set_sun_control_mode(getConfig().location.enabled || getConfig().timeSchedule.enabled);
  metrics_set_sun_mode(kSunModeUnknown);
  metrics_set_sun_state(false, false, false, 0, 0);

  if (getConfig().timeSchedule.enabled && ntp_has_valid_time()) {
    apply_time_schedule_brightness();
  } else if (!getConfig().location.enabled) {
    apply_manual_brightness_when_sun_control_disabled();
  } else {
    uint16_t minuteOfDay = 0;
    if (ensure_current_day_schedule(minuteOfDay)) {
      apply_candle_state(minuteOfDay);
      Serial.printf("[sun] startup offline schedule applied: mode=%s\n", sun_mode_name_local(s_lastModeMetric));
    } else {
      apply_manual_brightness_when_time_unavailable();
      Serial.println("[sun] startup offline mode waiting for valid time");
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

bool sun_position_is_candle_enabled() {
  const Config cfg = getConfig();
  if (cfg.timeSchedule.enabled && ntp_has_valid_time()) {
    return time_schedule_should_enable_now(cfg);
  }

  if ((!cfg.location.enabled && !cfg.timeSchedule.enabled) || !ntp_has_valid_time()) {
    return cfg.candleOn && cfg.brightness > 0;
  }

  return s_outputKnown ? s_candleEnabled : false;
}

bool sun_position_should_enable_now() {
  const Config cfg = getConfig();
  if (cfg.timeSchedule.enabled) {
    return time_schedule_should_enable_now(cfg);
  }

  if (!cfg.location.enabled) {
    return cfg.candleOn && cfg.brightness > 0;
  }

  if (!ntp_has_valid_time()) {
    return cfg.candleOn && cfg.brightness > 0;
  }

  uint16_t minuteOfDay = 0;
  if (!ensure_current_day_schedule(minuteOfDay)) {
    return false;
  }

  SunOfflineMode mode = SunOfflineMode::Night;
  if (!current_mode_from_elevation(mode)) {
    mode = sun_offline_mode_from_events(minuteOfDay % kMinutesPerDay, s_events);
  }
  return sun_offline_should_enable_candle(mode) && resolve_brightness_for_mode(mode) > 0;
}

bool sun_position_time_schedule_should_enable_now() {
  return time_schedule_should_enable_now(getConfig());
}

const char* sun_position_current_mode_name() {
  const Config cfg = getConfig();
  if (cfg.timeSchedule.enabled) {
    return ntp_has_valid_time() ? "time" : "waiting_time";
  }

  if (!cfg.location.enabled) {
    return "manual";
  }

  if (!ntp_has_valid_time()) {
    return "waiting_time";
  }

  uint16_t minuteOfDay = 0;
  if (!ensure_current_day_schedule(minuteOfDay)) {
    return "unknown";
  }

  SunOfflineMode mode = SunOfflineMode::Night;
  if (!current_mode_from_elevation(mode)) {
    mode = sun_offline_mode_from_events(minuteOfDay % kMinutesPerDay, s_events);
  }
  return sun_offline_mode_name(mode);
}
