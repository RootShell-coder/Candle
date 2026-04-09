#include <Arduino.h>

#include "config.h"
#include "freetros_module.h"
#include "i2c.h"

// Выполняет базовую инициализацию устройства и запускает фоновые задачи.
void setup() {
  Serial.begin(115200);
  configLoad();
  i2c_init();

  const Config& cfg = getConfig();
  const uint8_t startupBrightness = cfg.location.enabled ? 0 : (cfg.candleOn ? cfg.brightness : 0);
  i2c_set_brightness(startupBrightness);

  freertos_init();
}

// Основной цикл оставлен пустым, потому что логика работает в задачах FreeRTOS.
void loop() {
}
