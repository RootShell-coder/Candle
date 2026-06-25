#include "moon_led.h"

#include <Arduino.h>
#include <math.h>
#include <time.h>

#include "config.h"
#include "moon_phase.h"

#ifndef MOON_LED_PIN
#define MOON_LED_PIN -1
#endif

#ifndef MOON_LED_PWM_CHANNEL
#define MOON_LED_PWM_CHANNEL 6
#endif

#ifndef MOON_LED_PWM_FREQUENCY
#define MOON_LED_PWM_FREQUENCY 5000
#endif

#ifndef MOON_LED_PWM_RESOLUTION
#define MOON_LED_PWM_RESOLUTION 8
#endif

namespace {
bool s_initialized = false;
bool s_matrixActive = false;
uint8_t s_currentBrightness = 0;

struct RgbColor {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

uint8_t percent_to_byte(uint8_t percent) {
  return percent >= 100 ? 255 : static_cast<uint8_t>((static_cast<uint16_t>(percent) * 255U + 50U) / 100U);
}

RgbColor hsv_to_rgb(uint16_t hue, uint8_t valuePercent) {
  const float h = fmodf(static_cast<float>(hue), 360.0f) / 60.0f;
  const int sector = static_cast<int>(floorf(h));
  const float fraction = h - static_cast<float>(sector);
  const float v = static_cast<float>(percent_to_byte(valuePercent));
  const float p = 0.0f;
  const float q = v * (1.0f - fraction);
  const float t = v * fraction;

  switch (sector) {
    case 0:
      return { static_cast<uint8_t>(v), static_cast<uint8_t>(t), static_cast<uint8_t>(p) };
    case 1:
      return { static_cast<uint8_t>(q), static_cast<uint8_t>(v), static_cast<uint8_t>(p) };
    case 2:
      return { static_cast<uint8_t>(p), static_cast<uint8_t>(v), static_cast<uint8_t>(t) };
    case 3:
      return { static_cast<uint8_t>(p), static_cast<uint8_t>(q), static_cast<uint8_t>(v) };
    case 4:
      return { static_cast<uint8_t>(t), static_cast<uint8_t>(p), static_cast<uint8_t>(v) };
    default:
      return { static_cast<uint8_t>(v), static_cast<uint8_t>(p), static_cast<uint8_t>(q) };
  }
}

void write_output(uint8_t percent, uint16_t hue) {
  s_currentBrightness = percent > 100 ? 100 : percent;

#if MOON_LED_PIN >= 0
  if (!s_initialized) {
    return;
  }
  const RgbColor color = hsv_to_rgb(hue, s_currentBrightness);
  neopixelWrite(MOON_LED_PIN, color.r, color.g, color.b);
#endif
}
}  // namespace

void moon_led_init() {
  s_initialized = true;

#if MOON_LED_PIN >= 0
  pinMode(MOON_LED_PIN, OUTPUT);
#endif

  moon_led_apply();
}

void moon_led_apply() {
  const Config cfg = getConfig();
  if (!cfg.moonLed.enabled || cfg.moonLed.maxBrightness == 0 || !s_matrixActive) {
    write_output(0, cfg.moonLed.hue);
    return;
  }

  MoonPhase moon {};
  if (!moon_phase_calculate(time(nullptr), moon)) {
    write_output(0, cfg.moonLed.hue);
    return;
  }

  const double limited = fmax(0.0, fmin(1.0, moon.illumination));
  const uint8_t percent = static_cast<uint8_t>(
      lround(limited * static_cast<double>(cfg.moonLed.maxBrightness)));
  write_output(percent, cfg.moonLed.hue);
}

void moon_led_set_matrix_active(bool active) {
  if (s_matrixActive == active) {
    moon_led_apply();
    return;
  }

  s_matrixActive = active;
  moon_led_apply();
}

uint8_t moon_led_current_brightness() {
  return s_currentBrightness;
}

bool moon_led_hardware_enabled() {
#if MOON_LED_PIN >= 0
  return true;
#else
  return false;
#endif
}
