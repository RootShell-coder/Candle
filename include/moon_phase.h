#pragma once

#include <stdint.h>
#include <time.h>

enum class MoonPhaseType : uint8_t {
  NewMoon        = 0,
  WaxingCrescent = 1,
  FirstQuarter   = 2,
  WaxingGibbous  = 3,
  FullMoon       = 4,
  WaningGibbous  = 5,
  LastQuarter    = 6,
  WaningCrescent = 7,
};

struct MoonPhase {
  double        age_days;
  double        illumination;
  MoonPhaseType phase;
};

bool moon_phase_calculate(time_t unix_time_utc, MoonPhase& out);

MoonPhaseType moon_phase_from_age(double age_days);

const char* moon_phase_name(MoonPhaseType phase);
