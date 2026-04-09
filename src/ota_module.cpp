#include <atomic>
#include <string.h>

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>

#include "config.h"
#include "ota_module.h"
#include "wifi_module.h"

namespace {
std::atomic<bool> s_otaActive{false};

// Возвращает hostname для OTA с резервом на имя устройства Wi‑Fi.
const char* ota_hostname(const Config& cfg) {
  return strlen(cfg.ota.hostname) != 0 ? cfg.ota.hostname : cfg.wifi.devname;
}
}  // namespace

// Сообщает, выполняется ли OTA-обновление прямо сейчас.
bool ota_is_active() {
  return s_otaActive.load(std::memory_order_relaxed);
}

// Пытается поднять OTA сразу после готовности Wi‑Fi.
void ota_setup_with_wifi() {
  wifi_connect();
  if (WiFi.status() == WL_CONNECTED) {
    setupOTA();
  } else {
    Serial.println("[setup] WiFi is connecting, OTA will start later");
  }
}

// Настраивает обработчики ArduinoOTA и публикует сервис обновления.
void setupOTA() {
  const Config& cfg = getConfig();

  ArduinoOTA.setHostname(ota_hostname(cfg));
  ArduinoOTA.onStart([]() {
    s_otaActive.store(true, std::memory_order_relaxed);
    Serial.println("[ota] Start updating...");
  });
  ArduinoOTA.onEnd([]() {
    s_otaActive.store(false, std::memory_order_relaxed);
    Serial.println("[ota] End");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    const unsigned int percent = total == 0 ? 0 : (progress * 100U) / total;
    Serial.printf("[ota] Progress: %u%%\r", percent);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    s_otaActive.store(false, std::memory_order_relaxed);
    Serial.printf("[ota] Error[%u]: ", error);

    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });

  ArduinoOTA.setPasswordHash("81dc9bdb52d04dc20036dbd8313ed055");
  ArduinoOTA.begin();
  Serial.println("[ota] Ready");
}

// Обрабатывает входящие OTA-запросы из сетевого цикла.
void handleOTA() {
  ArduinoOTA.handle();
}
