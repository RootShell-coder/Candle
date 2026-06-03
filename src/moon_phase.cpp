#include "moon_phase.h"

#include <math.h>
#include <time.h>

// Юлианская дата известного новолуния: 6 января 2000, 18:14 UTC.
// JD = unix_timestamp / 86400.0 + 2440587.5 => ~2451550.264
static constexpr double kKnownNewMoonJD  = 2451550.264;

// Средний синодический период луны в сутках.
static constexpr double kSynodicPeriod = 29.53058770576;

// Переводит Unix timestamp в юлианскую дату.
static double unix_to_jd(time_t t) {
  return static_cast<double>(t) / 86400.0 + 2440587.5;
}

bool moon_phase_calculate(time_t unix_time_utc, MoonPhase& out) {
  if (unix_time_utc <= 0) {
    return false;
  }

  const double jd  = unix_to_jd(unix_time_utc);
  double age = fmod(jd - kKnownNewMoonJD, kSynodicPeriod);
  if (age < 0.0) {
    age += kSynodicPeriod;
  }

  out.age_days     = age;
  out.illumination = (1.0 - cos(2.0 * M_PI * age / kSynodicPeriod)) / 2.0;
  out.phase        = moon_phase_from_age(age);
  return true;
}

MoonPhaseType moon_phase_from_age(double age_days) {
  // Делим цикл на 8 равных секторов шириной ~3.69 суток.
  // Граница сектора смещена на половину ширины, чтобы новолуние было строго в центре.
  const double norm   = age_days / kSynodicPeriod; // [0..1)
  const int    sector = static_cast<int>(norm * 8.0 + 0.5) % 8;
  return static_cast<MoonPhaseType>(sector);
}

const char* moon_phase_name(MoonPhaseType phase) {
  switch (phase) {
    case MoonPhaseType::NewMoon:        return "New Moon";
    case MoonPhaseType::WaxingCrescent: return "Waxing Crescent";
    case MoonPhaseType::FirstQuarter:   return "First Quarter";
    case MoonPhaseType::WaxingGibbous:  return "Waxing Gibbous";
    case MoonPhaseType::FullMoon:       return "Full Moon";
    case MoonPhaseType::WaningGibbous:  return "Waning Gibbous";
    case MoonPhaseType::LastQuarter:    return "Last Quarter";
    case MoonPhaseType::WaningCrescent: return "Waning Crescent";
    default:                            return "Unknown";
  }
}
