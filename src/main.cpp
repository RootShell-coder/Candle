#include <Arduino.h>

#include "config.h"
#include "freetros_module.h"
#include "i2c.h"
#include "moon_led.h"
#include "wifi_module.h"

void setup() {
  Serial.begin(115200);
  delay(300);
  configLoad();
  moon_led_init();
  i2c_init();
  const Config cfg = getConfig();
  const bool captivePortalWillStart = !wifi_has_credentials();
  const uint8_t startupBrightness = captivePortalWillStart ? cfg.brightness
      : (cfg.candleOn ? cfg.brightness : 0);
  i2c_set_brightness(startupBrightness);
  freertos_init();
}

void loop() {
}
