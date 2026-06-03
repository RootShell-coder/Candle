#pragma once

#include <stdint.h>
#include <time.h>

// Восемь стандартных фаз лунного цикла.
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
  double        age_days;     // Возраст луны в сутках с последнего новолуния [0..29.53)
  double        illumination; // Доля освещённого диска [0..1]
  MoonPhaseType phase;        // Дискретная фаза (8 секторов)
};

// Вычисляет фазу луны для заданного UTC-времени (Unix timestamp).
// Возвращает false при unix_time_utc <= 0.
bool moon_phase_calculate(time_t unix_time_utc, MoonPhase& out);

// Определяет дискретную фазу по возрасту луны в сутках.
MoonPhaseType moon_phase_from_age(double age_days);

// Возвращает человекочитаемое название фазы.
const char* moon_phase_name(MoonPhaseType phase);
