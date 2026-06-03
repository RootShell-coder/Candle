#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <time.h>

enum class SunOfflineMode : uint8_t {
  Day = 0,
  Civil = 1,
  Nautical = 2,
  Astronomical = 3,
  Night = 4,
};

struct SunOfflinePosition {
  double azimuth_deg;
  double elevation_deg;
  double zenith_deg;
  double declination_deg;
  double equation_of_time_min;
};

struct SunOfflineEvents {
  bool has_sunrise;
  bool has_sunset;
  bool has_civil_twilight;
  bool has_nautical_twilight;
  bool has_astronomical_twilight;
  bool is_polar_day;
  bool is_polar_night;

  uint16_t sunrise_minute;
  uint16_t sunset_minute;
  uint16_t civil_begin_minute;
  uint16_t civil_end_minute;
  uint16_t nautical_begin_minute;
  uint16_t nautical_end_minute;
  uint16_t astronomical_begin_minute;
  uint16_t astronomical_end_minute;
};

bool sun_offline_calculate_position_utc(
    time_t unix_time_utc,
    double latitude_deg,
    double longitude_deg,
    SunOfflinePosition& out);

// NOAA solar position: азимут/высота солнца для точки в момент UTC.
// NOAA daily events: Восход/закат и сумерки для локальной даты.
bool sun_offline_calculate_events_local_day(
    int year,
    int month,
    int day,
    double latitude_deg,
    double longitude_deg,
    int tz_offset_minutes,
    SunOfflineEvents& out);

// Классификация режима по высоте солнца.
SunOfflineMode sun_offline_mode_from_elevation(double elevation_deg);

// Режим по расписанию дня для локальной минуты [0..1439].
SunOfflineMode sun_offline_mode_from_events(uint16_t minute_of_day, const SunOfflineEvents& events);

// Свеча включена во всех режимах, кроме Day.
bool sun_offline_should_enable_candle(SunOfflineMode mode);

// Имя режима для логов и API.
const char* sun_offline_mode_name(SunOfflineMode mode);
