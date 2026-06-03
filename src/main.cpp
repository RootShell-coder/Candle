#include <Arduino.h>

#include "config.h"
#include "freetros_module.h"
#include "i2c.h"

void setup() {
  Serial.begin(115200);
  delay(300);
  configLoad();
  i2c_init();
  const Config& cfg = getConfig();
  // При пустом SSID будет captive portal: включить свечу с предустановленной яркостью
  const bool captivePortalWillStart = (strlen(cfg.wifi.ssid) == 0);
  const uint8_t startupBrightness = captivePortalWillStart ? cfg.brightness
      : (cfg.location.enabled ? 0 : (cfg.candleOn ? cfg.brightness : 0));
  i2c_set_brightness(startupBrightness);
  freertos_init();
}

void loop() {
}
