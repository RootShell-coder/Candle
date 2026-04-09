#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "freetros_module.h"
#include "i2c.h"
#include "ntp_module.h"
#include "ota_module.h"
#include "sun_position.h"
#include "web_server.h"
#include "wifi_module.h"

namespace {
// Периоды опроса и размеры стеков для задач планировщика.
constexpr uint32_t kAnimFrameMs = 40;
constexpr uint32_t kNetworkRetryMs = 1000;
constexpr uint32_t kNetworkConnectedMs = 250;
constexpr uint32_t kCaptivePortalMs = 25;
constexpr uint32_t kOtaActiveMs = 20;
constexpr uint32_t kAnimTaskStack = 3072;
constexpr uint32_t kNetworkTaskStack = 6144;
constexpr BaseType_t kNetworkCore = 0;
constexpr BaseType_t kAnimCore = 1;

TaskHandle_t networkTaskHandle = nullptr;
TaskHandle_t i2cAnimTaskHandle = nullptr;
bool s_otaStarted = false;
bool s_webServerStarted = false;

// Запускает OTA после подключения к Wi‑Fi и web‑сервер также для captive portal.
void ensure_network_services_started() {
  const bool staConnected = WiFi.status() == WL_CONNECTED;
  const bool captivePortalActive = wifi_is_captive_portal_active();

  if (!staConnected && !captivePortalActive) {
    return;
  }

  if (staConnected && !s_otaStarted) {
    setupOTA();
    s_otaStarted = true;
  }

  if (!s_webServerStarted) {
    web_server_setup_with_wifi();
    s_webServerStarted = true;
  }
}
}  // namespace

extern void i2c_anim_task();

// Задача плавного обновления I2C-матрицы с фиксированным интервалом кадров.
static void i2c_anim_task_freertos(void* pvParameters) {
  (void)pvParameters;

  TickType_t lastWakeTime = xTaskGetTickCount();
  for (;;) {
    i2c_anim_task();
    vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(kAnimFrameMs));
  }
}

// Основная сетевая задача: reconnect Wi‑Fi, запуск сервисов, captive portal и обработка OTA.
static void network_service_task(void* pvParameters) {
  (void)pvParameters;

  for (;;) {
    wifi_handle_reconnect();
    ensure_network_services_started();
    ntp_handle();

    if (WiFi.status() == WL_CONNECTED && s_otaStarted) {
      handleOTA();
    }

    uint32_t delayMs = kNetworkRetryMs;
    if (ota_is_active()) {
      delayMs = kOtaActiveMs;
    } else if (wifi_is_captive_portal_active()) {
      delayMs = kCaptivePortalMs;
    } else if (WiFi.status() == WL_CONNECTED) {
      delayMs = kNetworkConnectedMs;
    }

    vTaskDelay(pdMS_TO_TICKS(delayMs));
  }
}

// Создаёт и распределяет задачи FreeRTOS по ядрам ESP32.
void freertos_init() {
  s_otaStarted = false;
  s_webServerStarted = false;
  ntp_init();
  sun_position_init();

  if (xTaskCreatePinnedToCore(
          network_service_task,
          "NetworkServiceTask",
          kNetworkTaskStack,
          nullptr,
          1,
          &networkTaskHandle,
          kNetworkCore) != pdPASS) {
    Serial.println("[rtos] failed to create NetworkServiceTask");
  }

  if (xTaskCreatePinnedToCore(
          i2c_anim_task_freertos,
          "I2CAnimTask",
          kAnimTaskStack,
          nullptr,
          2,
          &i2cAnimTaskHandle,
          kAnimCore) != pdPASS) {
    Serial.println("[rtos] failed to create I2CAnimTask");
  }
}
