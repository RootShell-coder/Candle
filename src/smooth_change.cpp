#include "smooth_change.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

#define SMOOTH_CHANGE_DURATION_MS 4000U

namespace {
portMUX_TYPE s_smoothChangeMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t s_currentValue = 0;
uint8_t s_startValue = 0;
uint8_t s_targetValue = 0;
uint32_t s_transitionStartedMs = 0;
bool s_initialized = false;

// Вычисляет текущее значение яркости внутри критической секции.
uint8_t interpolate_locked(uint32_t nowMs) {
  if (s_currentValue == s_targetValue) {
    return s_currentValue;
  }

  if (SMOOTH_CHANGE_DURATION_MS == 0U) {
    s_currentValue = s_targetValue;
    s_startValue = s_targetValue;
    return s_currentValue;
  }

  const uint32_t elapsedMs = nowMs - s_transitionStartedMs;
  if (elapsedMs >= SMOOTH_CHANGE_DURATION_MS) {
    s_currentValue = s_targetValue;
    s_startValue = s_targetValue;
    return s_currentValue;
  }

  const int32_t start = static_cast<int32_t>(s_startValue);
  const int32_t target = static_cast<int32_t>(s_targetValue);
  const int32_t delta = target - start;
  const int32_t scaled = start + ((delta * static_cast<int32_t>(elapsedMs))
      + static_cast<int32_t>(SMOOTH_CHANGE_DURATION_MS / 2U))
      / static_cast<int32_t>(SMOOTH_CHANGE_DURATION_MS);

  if (scaled <= 0) {
    s_currentValue = 0;
  } else if (scaled >= 255) {
    s_currentValue = 255;
  } else {
    s_currentValue = static_cast<uint8_t>(scaled);
  }

  return s_currentValue;
}
}  // namespace

// Инициализирует состояние плавного перехода начальной яркостью.
void smooth_change_init(uint8_t initialValue) {
  const uint32_t nowMs = millis();

  portENTER_CRITICAL(&s_smoothChangeMux);
  s_currentValue = initialValue;
  s_startValue = initialValue;
  s_targetValue = initialValue;
  s_transitionStartedMs = nowMs;
  s_initialized = true;
  portEXIT_CRITICAL(&s_smoothChangeMux);
}

// Задаёт новую целевую яркость и запускает переход к ней.
void smooth_change_set_target(uint8_t targetValue) {
  const uint32_t nowMs = millis();

  portENTER_CRITICAL(&s_smoothChangeMux);
  if (!s_initialized) {
    s_currentValue = 0;
    s_startValue = 0;
    s_targetValue = 0;
    s_transitionStartedMs = nowMs;
    s_initialized = true;
  }

  interpolate_locked(nowMs);

  if (s_targetValue != targetValue) {
    s_startValue = s_currentValue;
    s_targetValue = targetValue;
    s_transitionStartedMs = nowMs;

    if (SMOOTH_CHANGE_DURATION_MS == 0U) {
      s_currentValue = targetValue;
      s_startValue = targetValue;
    }
  }
  portEXIT_CRITICAL(&s_smoothChangeMux);
}

// Обновляет промежуточную яркость по времени и возвращает её.
uint8_t smooth_change_update() {
  const uint32_t nowMs = millis();
  uint8_t value = 0;

  portENTER_CRITICAL(&s_smoothChangeMux);
  if (!s_initialized) {
    s_currentValue = 0;
    s_startValue = 0;
    s_targetValue = 0;
    s_transitionStartedMs = nowMs;
    s_initialized = true;
  }

  value = interpolate_locked(nowMs);
  portEXIT_CRITICAL(&s_smoothChangeMux);

  return value;
}

// Возвращает текущую целевую яркость перехода.
uint8_t smooth_change_get_target() {
  uint8_t value = 0;

  portENTER_CRITICAL(&s_smoothChangeMux);
  value = s_targetValue;
  portEXIT_CRITICAL(&s_smoothChangeMux);

  return value;
}

// Показывает, продолжается ли сейчас плавное изменение яркости.
bool smooth_change_is_active() {
  bool active = false;

  portENTER_CRITICAL(&s_smoothChangeMux);
  active = (s_currentValue != s_targetValue);
  portEXIT_CRITICAL(&s_smoothChangeMux);

  return active;
}
