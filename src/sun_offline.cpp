#include "sun_offline.h"

#include <math.h>
#include <string.h>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kRadToDeg = 180.0 / kPi;
constexpr double kJulianUnixEpoch = 2440587.5;
constexpr uint16_t kMinutesPerDay = 24U * 60U;
constexpr double kZenithOfficial = 90.833;
constexpr double kZenithCivil = 96.0;
constexpr double kZenithNautical = 102.0;
constexpr double kZenithAstronomical = 108.0;

double normalize_deg(double value) {
  double out = fmod(value, 360.0);
  if (out < 0.0) {
    out += 360.0;
  }
  return out;
}

double normalize_minute(double minute) {
  double out = fmod(minute, static_cast<double>(kMinutesPerDay));
  if (out < 0.0) {
    out += static_cast<double>(kMinutesPerDay);
  }
  return out;
}

uint16_t to_minute_u16(double minute) {
  return static_cast<uint16_t>(normalize_minute(round(minute)));
}

int64_t days_from_civil(int year, unsigned month, unsigned day) {
  year -= month <= 2 ? 1 : 0;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const int adjustedMonth = static_cast<int>(month) + (month > 2 ? -3 : 9);
  const unsigned doy = (153U * static_cast<unsigned>(adjustedMonth) + 2U) / 5U + day - 1U;
  const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
  return static_cast<int64_t>(era) * 146097LL + static_cast<int64_t>(doe) - 719468LL;
}

double julian_day_from_unix_utc(time_t unix_time_utc) {
  return static_cast<double>(unix_time_utc) / 86400.0 + kJulianUnixEpoch;
}

void calculate_declination_and_eqtime(double julian_day, double& declination_deg, double& eqtime_min) {
  const double t = (julian_day - 2451545.0) / 36525.0;

  const double geom_mean_long = normalize_deg(280.46646 + t * (36000.76983 + t * 0.0003032));
  const double geom_mean_anom = normalize_deg(357.52911 + t * (35999.05029 - 0.0001537 * t));
  const double ecc_earth_orbit = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);

  const double mrad = geom_mean_anom * kDegToRad;
  const double sin_m = sin(mrad);
  const double sin_2m = sin(2.0 * mrad);
  const double sin_3m = sin(3.0 * mrad);

  const double sun_eq_center =
      sin_m * (1.914602 - t * (0.004817 + 0.000014 * t)) +
      sin_2m * (0.019993 - 0.000101 * t) +
      sin_3m * 0.000289;

  const double sun_true_long = geom_mean_long + sun_eq_center;
  const double omega = 125.04 - 1934.136 * t;
  const double sun_app_long = sun_true_long - 0.00569 - 0.00478 * sin(omega * kDegToRad);

  const double mean_obliq =
      23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0;
  const double obliq_corr = mean_obliq + 0.00256 * cos(omega * kDegToRad);

  const double obliq_rad = obliq_corr * kDegToRad;
  const double app_long_rad = sun_app_long * kDegToRad;

  declination_deg = asin(sin(obliq_rad) * sin(app_long_rad)) * kRadToDeg;

  const double y = tan(obliq_rad / 2.0);
  const double y2 = y * y;
  const double l0_rad = geom_mean_long * kDegToRad;

  const double eqtime_deg =
      y2 * sin(2.0 * l0_rad) -
      2.0 * ecc_earth_orbit * sin_m +
      4.0 * ecc_earth_orbit * y2 * sin_m * cos(2.0 * l0_rad) -
      0.5 * y2 * y2 * sin(4.0 * l0_rad) -
      1.25 * ecc_earth_orbit * ecc_earth_orbit * sin_2m;
  eqtime_min = 4.0 * eqtime_deg * kRadToDeg;
}

bool is_valid_lat_lon(double latitude_deg, double longitude_deg) {
  return latitude_deg >= -90.0 && latitude_deg <= 90.0 && longitude_deg >= -180.0 && longitude_deg <= 180.0;
}

bool is_in_wrapped_interval(uint16_t minute_of_day, uint16_t begin, uint16_t end) {
  if (begin == end) {
    return false;
  }
  if (begin < end) {
    return minute_of_day >= begin && minute_of_day < end;
  }
  return minute_of_day >= begin || minute_of_day < end;
}

bool calculate_event_minutes(
    double zenith_deg,
    double latitude_deg,
    double declination_deg,
    double eqtime_min,
    double longitude_deg,
    int tz_offset_minutes,
    uint16_t& morning_minute,
    uint16_t& evening_minute,
    bool& always_above,
    bool& always_below) {
  always_above = false;
  always_below = false;

  const double lat_rad = latitude_deg * kDegToRad;
  const double decl_rad = declination_deg * kDegToRad;
  const double cos_zenith = cos(zenith_deg * kDegToRad);
  const double cos_h = (cos_zenith / (cos(lat_rad) * cos(decl_rad))) - tan(lat_rad) * tan(decl_rad);

  if (cos_h > 1.0) {
    always_below = true;
    return false;
  }
  if (cos_h < -1.0) {
    always_above = true;
    return false;
  }

  const double h_deg = acos(cos_h) * kRadToDeg;
  const double morning = 720.0 - 4.0 * (longitude_deg + h_deg) - eqtime_min + static_cast<double>(tz_offset_minutes);
  const double evening = 720.0 - 4.0 * (longitude_deg - h_deg) - eqtime_min + static_cast<double>(tz_offset_minutes);

  morning_minute = to_minute_u16(morning);
  evening_minute = to_minute_u16(evening);
  return true;
}

} // namespace

bool sun_offline_calculate_position_utc(
    time_t unix_time_utc,
    double latitude_deg,
    double longitude_deg,
    SunOfflinePosition& out) {
  if (!is_valid_lat_lon(latitude_deg, longitude_deg)) {
    return false;
  }

  const double jd = julian_day_from_unix_utc(unix_time_utc);

  double declination_deg = 0.0;
  double eqtime_min = 0.0;
  calculate_declination_and_eqtime(jd, declination_deg, eqtime_min);

  const double utc_minutes = normalize_minute(static_cast<double>(unix_time_utc % 86400) / 60.0);
  const double true_solar_time = normalize_minute(utc_minutes + eqtime_min + 4.0 * longitude_deg);

  double hour_angle_deg = true_solar_time / 4.0 - 180.0;
  if (hour_angle_deg < -180.0) {
    hour_angle_deg += 360.0;
  }

  const double lat_rad = latitude_deg * kDegToRad;
  const double decl_rad = declination_deg * kDegToRad;
  const double ha_rad = hour_angle_deg * kDegToRad;

  double cos_zenith = sin(lat_rad) * sin(decl_rad) + cos(lat_rad) * cos(decl_rad) * cos(ha_rad);
  if (cos_zenith > 1.0) {
    cos_zenith = 1.0;
  }
  if (cos_zenith < -1.0) {
    cos_zenith = -1.0;
  }

  const double zenith_deg = acos(cos_zenith) * kRadToDeg;
  const double elevation_deg = 90.0 - zenith_deg;

  const double azimuth_deg = normalize_deg(
      atan2(
          sin(ha_rad),
          cos(ha_rad) * sin(lat_rad) - tan(decl_rad) * cos(lat_rad)) *
          kRadToDeg +
      180.0);

  out.azimuth_deg = azimuth_deg;
  out.elevation_deg = elevation_deg;
  out.zenith_deg = zenith_deg;
  out.declination_deg = declination_deg;
  out.equation_of_time_min = eqtime_min;
  return true;
}

bool sun_offline_calculate_events_local_day(
    int year,
    int month,
    int day,
    double latitude_deg,
    double longitude_deg,
    int tz_offset_minutes,
    SunOfflineEvents& out) {
  if (!is_valid_lat_lon(latitude_deg, longitude_deg) || month < 1 || month > 12 || day < 1 || day > 31) {
    return false;
  }

  memset(&out, 0, sizeof(out));

  const int64_t day_index = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  const double utc_noon_minutes = 12.0 * 60.0 - static_cast<double>(tz_offset_minutes);
  const double jd_noon =
      static_cast<double>(day_index) +
      kJulianUnixEpoch +
      utc_noon_minutes / 1440.0;

  double declination_deg = 0.0;
  double eqtime_min = 0.0;
  calculate_declination_and_eqtime(jd_noon, declination_deg, eqtime_min);

  bool rise_always_above = false;
  bool rise_always_below = false;
  out.has_sunrise = calculate_event_minutes(
      kZenithOfficial,
      latitude_deg,
      declination_deg,
      eqtime_min,
      longitude_deg,
      tz_offset_minutes,
      out.sunrise_minute,
      out.sunset_minute,
      rise_always_above,
      rise_always_below);
  out.has_sunset = out.has_sunrise;
  out.is_polar_day = rise_always_above;
  out.is_polar_night = rise_always_below;

  bool civil_always_above = false;
  bool civil_always_below = false;
  out.has_civil_twilight = calculate_event_minutes(
      kZenithCivil,
      latitude_deg,
      declination_deg,
      eqtime_min,
      longitude_deg,
      tz_offset_minutes,
      out.civil_begin_minute,
      out.civil_end_minute,
      civil_always_above,
      civil_always_below);

  bool nautical_always_above = false;
  bool nautical_always_below = false;
  out.has_nautical_twilight = calculate_event_minutes(
      kZenithNautical,
      latitude_deg,
      declination_deg,
      eqtime_min,
      longitude_deg,
      tz_offset_minutes,
      out.nautical_begin_minute,
      out.nautical_end_minute,
      nautical_always_above,
      nautical_always_below);

  bool astronomical_always_above = false;
  bool astronomical_always_below = false;
  out.has_astronomical_twilight = calculate_event_minutes(
      kZenithAstronomical,
      latitude_deg,
      declination_deg,
      eqtime_min,
      longitude_deg,
      tz_offset_minutes,
      out.astronomical_begin_minute,
      out.astronomical_end_minute,
      astronomical_always_above,
      astronomical_always_below);

  (void)civil_always_above;
  (void)civil_always_below;
  (void)nautical_always_above;
  (void)nautical_always_below;
  (void)astronomical_always_above;
  (void)astronomical_always_below;
  return true;
}

SunOfflineMode sun_offline_mode_from_elevation(double elevation_deg) {
  if (elevation_deg >= 0.0) {
    return SunOfflineMode::Day;
  }
  if (elevation_deg >= -6.0) {
    return SunOfflineMode::Civil;
  }
  if (elevation_deg >= -12.0) {
    return SunOfflineMode::Nautical;
  }
  if (elevation_deg >= -18.0) {
    return SunOfflineMode::Astronomical;
  }
  return SunOfflineMode::Night;
}

SunOfflineMode sun_offline_mode_from_events(uint16_t minute_of_day, const SunOfflineEvents& events) {
  const uint16_t m = minute_of_day % kMinutesPerDay;

  if (events.is_polar_day) {
    return SunOfflineMode::Day;
  }
  if (events.is_polar_night) {
    return SunOfflineMode::Night;
  }

  if (events.has_sunrise && events.has_sunset && is_in_wrapped_interval(m, events.sunrise_minute, events.sunset_minute)) {
    return SunOfflineMode::Day;
  }

  if (events.has_civil_twilight &&
      events.has_sunrise && events.has_sunset &&
      (is_in_wrapped_interval(m, events.civil_begin_minute, events.sunrise_minute) ||
       is_in_wrapped_interval(m, events.sunset_minute, events.civil_end_minute))) {
    return SunOfflineMode::Civil;
  }

  if (events.has_nautical_twilight && events.has_civil_twilight &&
      (is_in_wrapped_interval(m, events.nautical_begin_minute, events.civil_begin_minute) ||
       is_in_wrapped_interval(m, events.civil_end_minute, events.nautical_end_minute))) {
    return SunOfflineMode::Nautical;
  }

  if (events.has_astronomical_twilight && events.has_nautical_twilight &&
      (is_in_wrapped_interval(m, events.astronomical_begin_minute, events.nautical_begin_minute) ||
       is_in_wrapped_interval(m, events.nautical_end_minute, events.astronomical_end_minute))) {
    return SunOfflineMode::Astronomical;
  }

  return SunOfflineMode::Night;
}

bool sun_offline_should_enable_candle(SunOfflineMode mode) {
  return mode != SunOfflineMode::Day;
}

const char* sun_offline_mode_name(SunOfflineMode mode) {
  switch (mode) {
    case SunOfflineMode::Day:
      return "day";
    case SunOfflineMode::Civil:
      return "civil";
    case SunOfflineMode::Nautical:
      return "nautical";
    case SunOfflineMode::Astronomical:
      return "astronomical";
    case SunOfflineMode::Night:
      return "night";
    default:
      return "unknown";
  }
}
