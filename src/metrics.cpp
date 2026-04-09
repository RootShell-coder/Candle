#include "metrics.h"

#include <Arduino.h>
#include <WiFi.h>
#include <atomic>
#include <time.h>

#include "config.h"
#include "ota_module.h"

namespace {
constexpr uint32_t kDefaultI2cClockHz = 100000;
constexpr size_t kI2cStageCount = static_cast<size_t>(I2cMetricStage::Count);
constexpr size_t kI2cErrorCodeCount = 6;

std::atomic<uint32_t> s_wifiConnectAttemptsTotal{0};
std::atomic<uint32_t> s_wifiConnectedTotal{0};
std::atomic<uint32_t> s_wifiDisconnectsTotal{0};
std::atomic<uint32_t> s_httpRequestsTotal{0};
std::atomic<uint32_t> s_prometheusScrapesTotal{0};
std::atomic<int32_t> s_ntpDriftSeconds{0};
std::atomic<uint32_t> s_ntpLastSuccessEpoch{0};
std::atomic<uint32_t> s_ntpFailuresTotal{0};
std::atomic<uint32_t> s_i2cFramesRenderedTotal{0};
std::atomic<uint32_t> s_i2cFramesSkippedTotal{0};
std::atomic<uint32_t> s_i2cErrorsTotal{0};
std::atomic<uint32_t> s_i2cRecoveriesTotal{0};
std::atomic<uint32_t> s_i2cRecoveryFailuresTotal{0};
std::atomic<uint32_t> s_i2cClockHz{kDefaultI2cClockHz};
std::atomic<uint32_t> s_brightness{255};
std::atomic<uint32_t> s_i2cStageAttemptsTotal[kI2cStageCount]{};
std::atomic<uint32_t> s_i2cStageSuccessTotal[kI2cStageCount]{};
std::atomic<uint32_t> s_i2cStageFailuresTotal[kI2cStageCount]{};
std::atomic<uint32_t> s_i2cStageRetriesTotal[kI2cStageCount]{};
std::atomic<uint32_t> s_i2cStageBytesWrittenTotal[kI2cStageCount]{};
std::atomic<uint32_t> s_i2cBusBytesWrittenTotal{0};
std::atomic<uint32_t> s_i2cEndTransmissionErrorTotal[kI2cErrorCodeCount]{};
std::atomic<int32_t> s_i2cLastErrorCode{0};
std::atomic<uint32_t> s_i2cLastErrorStage{static_cast<uint32_t>(I2cMetricStage::Count)};
std::atomic<uint32_t> s_i2cLastErrorEpoch{0};
std::atomic<uint32_t> s_i2cLastSuccessEpoch{0};
std::atomic<uint32_t> s_i2cLastRecoveryEpoch{0};
std::atomic<uint32_t> s_i2cLastRecoveryDurationMs{0};
std::atomic<uint32_t> s_i2cMaxRecoveryDurationMs{0};
std::atomic<uint32_t> s_i2cCurrentPage{0};
std::atomic<uint32_t> s_i2cConsecutiveErrors{0};
std::atomic<uint32_t> s_i2cConsecutiveSuccess{0};
std::atomic<uint32_t> s_i2cMaxConsecutiveErrors{0};
std::atomic<uint32_t> s_i2cMaxConsecutiveSuccess{0};
std::atomic<uint32_t> s_i2cLastFrameChangedBytes{0};
std::atomic<uint32_t> s_i2cMaxFrameChangedBytes{0};
std::atomic<uint32_t> s_i2cChangedBytesTotal{0};
std::atomic<uint32_t> s_i2cLastRenderDurationUs{0};
std::atomic<uint32_t> s_i2cMaxRenderDurationUs{0};
std::atomic<uint32_t> s_i2cClockFallbackActive{0};
std::atomic<uint32_t> s_i2cClockFallbacksTotal{0};
std::atomic<uint32_t> s_i2cClockRestoresTotal{0};
std::atomic<uint32_t> s_sunCacheLoaded{0};
std::atomic<uint32_t> s_sunCandleEnabled{0};
std::atomic<uint32_t> s_sunFetchInProgress{0};
std::atomic<uint32_t> s_sunTargetBrightness{0};
std::atomic<uint32_t> s_sunAttemptsToday{0};
std::atomic<uint32_t> s_sunCurrentMode{0};
std::atomic<uint32_t> s_sunControlMode{0};
std::atomic<uint32_t> s_sunLastUpdateEpoch{0};
std::atomic<uint32_t> s_sunSunriseMinutes{0};
std::atomic<uint32_t> s_sunSunsetMinutes{0};
std::atomic<uint32_t> s_sunCivilTwilightBeginMinutes{0};
std::atomic<uint32_t> s_sunCivilTwilightEndMinutes{0};
std::atomic<uint32_t> s_sunNauticalTwilightBeginMinutes{0};
std::atomic<uint32_t> s_sunNauticalTwilightEndMinutes{0};
std::atomic<uint32_t> s_sunAstronomicalTwilightBeginMinutes{0};
std::atomic<uint32_t> s_sunAstronomicalTwilightEndMinutes{0};
std::atomic<uint32_t> s_sunRefreshRequestsTotal{0};
std::atomic<uint32_t> s_sunUpdateSuccessTotal{0};
std::atomic<uint32_t> s_sunUpdateFailuresTotal{0};

// Преобразует enum стадии I2C в индекс массива метрик.
size_t i2c_stage_index(I2cMetricStage stage) {
  return static_cast<size_t>(stage);
}

// Возвращает короткое имя стадии I2C для Prometheus label.
const char* i2c_stage_name(I2cMetricStage stage) {
  switch (stage) {
    case I2cMetricStage::Init:
      return "init";
    case I2cMetricStage::SelectPage:
      return "select_page";
    case I2cMetricStage::WriteChunk:
      return "write_chunk";
    case I2cMetricStage::WriteFrame:
      return "write_frame";
    case I2cMetricStage::FlipPage:
      return "flip_page";
    case I2cMetricStage::Recover:
      return "recover";
    case I2cMetricStage::Count:
      return "none";
  }
  return "unknown";
}

// Отмечает стадии, которые реально выполняют обмен по шине.
bool i2c_stage_is_bus_io(I2cMetricStage stage) {
  return stage == I2cMetricStage::SelectPage || stage == I2cMetricStage::WriteChunk || stage == I2cMetricStage::FlipPage;
}

// Сопоставляет код `Wire.endTransmission()` с индексом массива счётчиков.
size_t i2c_error_code_index(int code) {
  switch (code) {
    case 1:
      return 0;
    case 2:
      return 1;
    case 3:
      return 2;
    case 4:
      return 3;
    case 5:
      return 4;
    default:
      return 5;
  }
}

// Возвращает текстовую расшифровку кода ошибки I2C.
const char* i2c_error_code_name(int code) {
  switch (code) {
    case 0:
      return "ok";
    case 1:
      return "data_too_long";
    case 2:
      return "address_nack";
    case 3:
      return "data_nack";
    case 4:
      return "other_error";
    case 5:
      return "timeout";
    default:
      return "unknown";
  }
}

// Преобразует индекс метрики обратно в исходный код ошибки.
int i2c_error_code_value_from_index(size_t index) {
  static const int kCodes[kI2cErrorCodeCount] = {1, 2, 3, 4, 5, -1};
  return (index < kI2cErrorCodeCount) ? kCodes[index] : -1;
}

// Атомарно обновляет максимум без потери параллельных записей.
void update_max(std::atomic<uint32_t>& target, uint32_t value) {
  uint32_t current = target.load(std::memory_order_relaxed);
  while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
  }
}

// Возвращает текущее Unix-время или 0, если оно ещё не синхронизировано.
uint32_t unix_time_now() {
  const time_t nowEpoch = time(nullptr);
  return nowEpoch > 0 ? static_cast<uint32_t>(nowEpoch) : 0U;
}

// Экранирует label так, чтобы его можно было безопасно отдать в Prometheus.
String escape_prometheus_label(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 8);

  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c == '\\' || c == '"') {
      escaped += '\\';
      escaped += c;
      continue;
    }
    if (c == '\n' || c == '\r') {
      escaped += ' ';
      continue;
    }
    escaped += c;
  }

  return escaped;
}

// Преобразует код солнечного режима в читаемое имя.
const char* sun_mode_name(uint32_t mode) {
  switch (mode) {
    case 1:
      return "day";
    case 2:
      return "civil";
    case 3:
      return "nautical";
    case 4:
      return "astronomical";
    case 5:
      return "night";
    case 6:
      return "disabled";
    default:
      return "unknown";
  }
}

// Преобразует источник управления яркостью в строку для label.
const char* sun_control_mode_name(uint32_t mode) {
  switch (mode) {
    case 1:
      return "sun";
    case 0:
    default:
      return "manual";
  }
}

// Добавляет HELP/TYPE заголовок Prometheus-метрики.
void append_meta(String& out, const char* name, const char* help, const char* type) {
  out += F("# HELP ");
  out += name;
  out += ' ';
  out += help;
  out += '\n';
  out += F("# TYPE ");
  out += name;
  out += ' ';
  out += type;
  out += '\n';
}

// Добавляет беззнаковую метрику в текстовый ответ.
void append_uint_metric(String& out, const char* name, uint32_t value) {
  out += name;
  out += ' ';
  out += String(value);
  out += '\n';
}

// Добавляет знаковую метрику в текстовый ответ.
void append_int_metric(String& out, const char* name, int32_t value) {
  out += name;
  out += ' ';
  out += String(value);
  out += '\n';
}

// Добавляет метрику с плавающей точкой в текстовый ответ.
void append_float_metric(String& out, const char* name, float value, unsigned int decimals = 3) {
  out += name;
  out += ' ';
  out += String(static_cast<double>(value), decimals);
  out += '\n';
}

// Печатает значения по стадиям I2C как набор label-метрик.
void append_i2c_stage_metric(String& out, const char* name, std::atomic<uint32_t> values[kI2cStageCount]) {
  for (size_t i = 0; i < kI2cStageCount; ++i) {
    out += name;
    out += "{stage=\"";
    out += i2c_stage_name(static_cast<I2cMetricStage>(i));
    out += "\"} ";
    out += String(values[i].load(std::memory_order_relaxed));
    out += '\n';
  }
}

// Печатает распределение ошибок `Wire.endTransmission()` по кодам.
void append_i2c_error_metric(String& out, const char* name, std::atomic<uint32_t> values[kI2cErrorCodeCount]) {
  for (size_t i = 0; i < kI2cErrorCodeCount; ++i) {
    const int code = i2c_error_code_value_from_index(i);
    out += name;
    out += "{code=\"";
    if (code >= 0) {
      out += String(code);
    } else {
      out += "unknown";
    }
    out += "\",name=\"";
    out += i2c_error_code_name(code);
    out += "\"} ";
    out += String(values[i].load(std::memory_order_relaxed));
    out += '\n';
  }
}
}  // namespace

// Увеличивает счётчик попыток подключения к Wi‑Fi.
void metrics_record_wifi_connect_attempt() {
  s_wifiConnectAttemptsTotal.fetch_add(1, std::memory_order_relaxed);
}

// Увеличивает счётчик успешных подключений к Wi‑Fi.
void metrics_record_wifi_connected() {
  s_wifiConnectedTotal.fetch_add(1, std::memory_order_relaxed);
}

// Увеличивает счётчик разрывов Wi‑Fi-соединения.
void metrics_record_wifi_disconnect() {
  s_wifiDisconnectsTotal.fetch_add(1, std::memory_order_relaxed);
}

// Увеличивает счётчик динамических HTTP-запросов.
void metrics_record_http_request() {
  s_httpRequestsTotal.fetch_add(1, std::memory_order_relaxed);
}

// Отмечает очередное чтение endpoint `/metrics`.
void metrics_record_prometheus_scrape() {
  s_prometheusScrapesTotal.fetch_add(1, std::memory_order_relaxed);
}

// Сохраняет результат успешной NTP-синхронизации.
void metrics_record_ntp_sync(int32_t driftSeconds, uint32_t syncedAtEpoch) {
  s_ntpDriftSeconds.store(driftSeconds, std::memory_order_relaxed);
  s_ntpLastSuccessEpoch.store(syncedAtEpoch, std::memory_order_relaxed);
}

// Увеличивает счётчик неудачных NTP-запросов.
void metrics_record_ntp_failure() {
  s_ntpFailuresTotal.fetch_add(1, std::memory_order_relaxed);
}

// Фиксирует успешную отправку кадра в LED-драйвер.
void metrics_record_i2c_frame_rendered() {
  s_i2cFramesRenderedTotal.fetch_add(1, std::memory_order_relaxed);
  s_i2cLastSuccessEpoch.store(unix_time_now(), std::memory_order_relaxed);
}

// Фиксирует пропуск кадра без ошибки, если изображение не изменилось.
void metrics_record_i2c_frame_skipped() {
  s_i2cFramesSkippedTotal.fetch_add(1, std::memory_order_relaxed);
  s_i2cLastSuccessEpoch.store(unix_time_now(), std::memory_order_relaxed);
}

// Увеличивает общий счётчик ошибок рендера по I2C.
void metrics_record_i2c_error() {
  s_i2cErrorsTotal.fetch_add(1, std::memory_order_relaxed);
}

// Регистрирует попытку выполнения конкретной стадии I2C-обмена.
void metrics_record_i2c_stage_attempt(I2cMetricStage stage) {
  s_i2cStageAttemptsTotal[i2c_stage_index(stage)].fetch_add(1, std::memory_order_relaxed);
}

// Регистрирует успешное завершение стадии I2C с учётом байт и ретраев.
void metrics_record_i2c_stage_success(I2cMetricStage stage, uint16_t bytesWritten, uint8_t retriesUsed) {
  const size_t index = i2c_stage_index(stage);
  s_i2cStageSuccessTotal[index].fetch_add(1, std::memory_order_relaxed);
  s_i2cStageRetriesTotal[index].fetch_add(retriesUsed, std::memory_order_relaxed);
  s_i2cStageBytesWrittenTotal[index].fetch_add(bytesWritten, std::memory_order_relaxed);

  if (i2c_stage_is_bus_io(stage)) {
    s_i2cBusBytesWrittenTotal.fetch_add(bytesWritten, std::memory_order_relaxed);
  }
}

// Регистрирует неудачу стадии I2C и использованные повторы.
void metrics_record_i2c_stage_failure(I2cMetricStage stage, uint8_t retriesUsed) {
  const size_t index = i2c_stage_index(stage);
  s_i2cStageFailuresTotal[index].fetch_add(1, std::memory_order_relaxed);
  s_i2cStageRetriesTotal[index].fetch_add(retriesUsed, std::memory_order_relaxed);
}

// Запоминает последнюю низкоуровневую ошибку шины и её стадию.
void metrics_record_i2c_bus_error(I2cMetricStage stage, int errorCode) {
  s_i2cLastErrorStage.store(static_cast<uint32_t>(stage), std::memory_order_relaxed);
  s_i2cLastErrorCode.store(errorCode, std::memory_order_relaxed);
  s_i2cLastErrorEpoch.store(unix_time_now(), std::memory_order_relaxed);
  s_i2cEndTransmissionErrorTotal[i2c_error_code_index(errorCode)].fetch_add(1, std::memory_order_relaxed);
}

// Сохраняет результат последней процедуры восстановления I2C.
void metrics_record_i2c_recover(bool success, uint32_t durationMs) {
  s_i2cLastRecoveryDurationMs.store(durationMs, std::memory_order_relaxed);
  update_max(s_i2cMaxRecoveryDurationMs, durationMs);
  s_i2cLastRecoveryEpoch.store(unix_time_now(), std::memory_order_relaxed);

  if (success) {
    s_i2cRecoveriesTotal.fetch_add(1, std::memory_order_relaxed);
    s_i2cLastSuccessEpoch.store(unix_time_now(), std::memory_order_relaxed);
  } else {
    s_i2cRecoveryFailuresTotal.fetch_add(1, std::memory_order_relaxed);
  }
}

// Обновляет текущие счётчики стабильности и активную страницу матрицы.
void metrics_set_i2c_state(uint8_t currentPage, uint32_t consecutiveErrors, uint32_t consecutiveSuccess) {
  s_i2cCurrentPage.store(currentPage, std::memory_order_relaxed);
  s_i2cConsecutiveErrors.store(consecutiveErrors, std::memory_order_relaxed);
  s_i2cConsecutiveSuccess.store(consecutiveSuccess, std::memory_order_relaxed);
  update_max(s_i2cMaxConsecutiveErrors, consecutiveErrors);
  update_max(s_i2cMaxConsecutiveSuccess, consecutiveSuccess);
}

// Запоминает, сколько байт изменилось в последнем кадре.
void metrics_record_i2c_frame_change(uint16_t changedBytes) {
  s_i2cLastFrameChangedBytes.store(changedBytes, std::memory_order_relaxed);
  s_i2cChangedBytesTotal.fetch_add(changedBytes, std::memory_order_relaxed);
  update_max(s_i2cMaxFrameChangedBytes, changedBytes);
}

// Обновляет длительность последней попытки рендера кадра.
void metrics_record_i2c_render_duration(uint32_t durationUs) {
  s_i2cLastRenderDurationUs.store(durationUs, std::memory_order_relaxed);
  update_max(s_i2cMaxRenderDurationUs, durationUs);
}

// Переключает флаг fallback-частоты I2C и ведёт счётчик переходов.
void metrics_record_i2c_clock_fallback(bool active) {
  const uint32_t nextValue = active ? 1U : 0U;
  const uint32_t previousValue = s_i2cClockFallbackActive.exchange(nextValue, std::memory_order_relaxed);

  if (nextValue != previousValue) {
    if (active) {
      s_i2cClockFallbacksTotal.fetch_add(1, std::memory_order_relaxed);
    } else {
      s_i2cClockRestoresTotal.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

// Сохраняет текущую частоту шины I2C для диагностики.
void metrics_set_i2c_clock_hz(uint32_t hz) {
  s_i2cClockHz.store(hz, std::memory_order_relaxed);
}

// Сохраняет текущую целевую яркость LED-матрицы.
void metrics_set_brightness(uint8_t value) {
  s_brightness.store(value, std::memory_order_relaxed);
}

// Обновляет агрегированное состояние солнечного контроллера.
void metrics_set_sun_state(bool cacheLoaded, bool candleEnabled, bool fetchInProgress, uint8_t targetBrightness, uint8_t attemptsToday) {
  s_sunCacheLoaded.store(cacheLoaded ? 1U : 0U, std::memory_order_relaxed);
  s_sunCandleEnabled.store(candleEnabled ? 1U : 0U, std::memory_order_relaxed);
  s_sunFetchInProgress.store(fetchInProgress ? 1U : 0U, std::memory_order_relaxed);
  s_sunTargetBrightness.store(targetBrightness, std::memory_order_relaxed);
  s_sunAttemptsToday.store(attemptsToday, std::memory_order_relaxed);
}

// Сохраняет текущий режим дня по солнечному расписанию.
void metrics_set_sun_mode(uint8_t currentMode) {
  s_sunCurrentMode.store(currentMode, std::memory_order_relaxed);
}

// Сохраняет источник управления яркостью: ручной или солнечный.
void metrics_set_sun_control_mode(bool sunControlled) {
  s_sunControlMode.store(sunControlled ? 1U : 0U, std::memory_order_relaxed);
}

// Сохраняет кэшированное расписание солнца для экспорта в метрики.
void metrics_set_sun_schedule(uint32_t updatedAtEpoch,
                              uint16_t sunriseMinutes,
                              uint16_t sunsetMinutes,
                              uint16_t civilTwilightBeginMinutes,
                              uint16_t civilTwilightEndMinutes,
                              uint16_t nauticalTwilightBeginMinutes,
                              uint16_t nauticalTwilightEndMinutes,
                              uint16_t astronomicalTwilightBeginMinutes,
                              uint16_t astronomicalTwilightEndMinutes) {
  s_sunLastUpdateEpoch.store(updatedAtEpoch, std::memory_order_relaxed);
  s_sunSunriseMinutes.store(sunriseMinutes, std::memory_order_relaxed);
  s_sunSunsetMinutes.store(sunsetMinutes, std::memory_order_relaxed);
  s_sunCivilTwilightBeginMinutes.store(civilTwilightBeginMinutes, std::memory_order_relaxed);
  s_sunCivilTwilightEndMinutes.store(civilTwilightEndMinutes, std::memory_order_relaxed);
  s_sunNauticalTwilightBeginMinutes.store(nauticalTwilightBeginMinutes, std::memory_order_relaxed);
  s_sunNauticalTwilightEndMinutes.store(nauticalTwilightEndMinutes, std::memory_order_relaxed);
  s_sunAstronomicalTwilightBeginMinutes.store(astronomicalTwilightBeginMinutes, std::memory_order_relaxed);
  s_sunAstronomicalTwilightEndMinutes.store(astronomicalTwilightEndMinutes, std::memory_order_relaxed);
}

// Увеличивает счётчик запланированных обновлений солнечного кэша.
void metrics_record_sun_refresh_request() {
  s_sunRefreshRequestsTotal.fetch_add(1, std::memory_order_relaxed);
}

// Увеличивает счётчик успешных обновлений `sun_position.json`.
void metrics_record_sun_update_success() {
  s_sunUpdateSuccessTotal.fetch_add(1, std::memory_order_relaxed);
}

// Увеличивает счётчик неудачных обновлений солнечного расписания.
void metrics_record_sun_update_failure() {
  s_sunUpdateFailuresTotal.fetch_add(1, std::memory_order_relaxed);
}

// Формирует полный Prometheus-ответ со всеми диагностическими метриками.
String metrics_render_prometheus() {
  const Config& cfg = getConfig();
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  const long rssi = wifiConnected ? WiFi.RSSI() : -127;
  const String ip = wifiConnected ? WiFi.localIP().toString() : String("0.0.0.0");
  const time_t nowEpoch = time(nullptr);
  const uint32_t lastErrorStageValue = s_i2cLastErrorStage.load(std::memory_order_relaxed);
  const I2cMetricStage lastErrorStage =
      lastErrorStageValue < kI2cStageCount ? static_cast<I2cMetricStage>(lastErrorStageValue) : I2cMetricStage::Count;
  const int32_t lastErrorCode = s_i2cLastErrorCode.load(std::memory_order_relaxed);

  String out;
  out.reserve(10000);

  // `candle_up` — признак того, что прошивка жива и endpoint `/metrics` отвечает.
  // Всегда отдает 1, пока устройство работает.
  append_meta(out, "candle_up", "Firmware is running.", "gauge");
  append_uint_metric(out, "candle_up", 1);

  // `candle_uptime_seconds` — время непрерывной работы устройства после последней загрузки.
  // Значение берется из `millis()` и переводится в секунды.
  append_meta(out, "candle_uptime_seconds", "Seconds since boot.", "gauge");
  append_float_metric(out, "candle_uptime_seconds", millis() / 1000.0f, 3);

  // `candle_heap_free_bytes` — сколько свободной RAM доступно сейчас.
  // Помогает отслеживать утечки памяти и нехватку heap.
  append_meta(out, "candle_heap_free_bytes", "Currently free heap in bytes.", "gauge");
  append_uint_metric(out, "candle_heap_free_bytes", ESP.getFreeHeap());

  // `candle_heap_min_free_bytes` — минимальный остаток свободной RAM с момента старта.
  // Полезно для оценки пикового потребления памяти.
  append_meta(out, "candle_heap_min_free_bytes", "Minimum free heap seen since boot in bytes.", "gauge");
  append_uint_metric(out, "candle_heap_min_free_bytes", ESP.getMinFreeHeap());

  // `candle_current_unixtime` — текущее системное время микроконтроллера в формате Unix time.
  // После успешной NTP-синхронизации позволяет видеть актуальные часы устройства в `/metrics`.
  append_meta(out, "candle_current_unixtime", "Current microcontroller system time as Unix timestamp.", "gauge");
  append_uint_metric(out, "candle_current_unixtime", nowEpoch > 0 ? static_cast<uint32_t>(nowEpoch) : 0U);

  // `candle_wifi_connected` — текущее состояние Wi‑Fi.
  // 1 = подключено к сети, 0 = соединения нет.
  append_meta(out, "candle_wifi_connected", "Wi-Fi connection state: 1=connected, 0=disconnected.", "gauge");
  append_uint_metric(out, "candle_wifi_connected", wifiConnected ? 1U : 0U);

  // `candle_wifi_rssi_dbm` — уровень сигнала Wi‑Fi в dBm.
  // Чем ближе к 0, тем лучше; при отсутствии подключения отдается -127.
  append_meta(out, "candle_wifi_rssi_dbm", "Wi-Fi RSSI in dBm, or -127 when disconnected.", "gauge");
  out += "candle_wifi_rssi_dbm ";
  out += String(rssi);
  out += '\n';

  // `candle_ntp_drift_seconds` — поправка, которую пришлось внести в системные часы на последней удачной синхронизации.
  // Положительное значение означает, что локальные часы отставали, отрицательное — спешили.
  append_meta(out, "candle_ntp_drift_seconds", "Clock correction applied on the last successful NTP sync in seconds.", "gauge");
  append_int_metric(out, "candle_ntp_drift_seconds", s_ntpDriftSeconds.load(std::memory_order_relaxed));

  // `candle_ntp_last_success_unixtime` — Unix time последней успешной синхронизации по NTP.
  // Удобно для контроля, насколько давно устройство сверяло часы.
  append_meta(out, "candle_ntp_last_success_unixtime", "Unix timestamp of the last successful NTP synchronization.", "gauge");
  append_uint_metric(out, "candle_ntp_last_success_unixtime", s_ntpLastSuccessEpoch.load(std::memory_order_relaxed));

  // `candle_ota_active` — идет ли сейчас OTA-обновление прошивки.
  // 1 = обновление активно, 0 = OTA не выполняется.
  append_meta(out, "candle_ota_active", "OTA status: 1 while firmware update is active.", "gauge");
  append_uint_metric(out, "candle_ota_active", ota_is_active() ? 1U : 0U);

  // `candle_i2c_brightness` — текущая яркость матрицы, которую использует драйвер анимации.
  // Диапазон: от 0 до 255.
  append_meta(out, "candle_i2c_brightness", "Current LED brightness in range 0..255.", "gauge");
  append_uint_metric(out, "candle_i2c_brightness", s_brightness.load(std::memory_order_relaxed));

  // `candle_sun_cache_loaded` — удалось ли загрузить график солнца из `/sun_position.json`.
  // 1 = данные из файла доступны и используются, 0 = кэш отсутствует или невалиден.
  append_meta(out, "candle_sun_cache_loaded", "1 when sunrise/sunset schedule was successfully loaded from /sun_position.json.", "gauge");
  append_uint_metric(out, "candle_sun_cache_loaded", s_sunCacheLoaded.load(std::memory_order_relaxed));

  // `candle_sun_fetch_in_progress` — выполняется ли сейчас фоновое обновление файла `sun_position.json`.
  // Помогает понять, когда устройство делает HTTPS-запрос к API.
  append_meta(out, "candle_sun_fetch_in_progress", "1 while a background sunrise-sunset refresh request is running.", "gauge");
  append_uint_metric(out, "candle_sun_fetch_in_progress", s_sunFetchInProgress.load(std::memory_order_relaxed));

  // `candle_sun_candle_enabled` — должен ли свет быть включен по текущему расписанию солнца.
  // 1 = яркость больше нуля, 0 = дневное выключение.
  append_meta(out, "candle_sun_candle_enabled", "1 when the sun schedule currently wants the candle output enabled.", "gauge");
  append_uint_metric(out, "candle_sun_candle_enabled", s_sunCandleEnabled.load(std::memory_order_relaxed));

  // `candle_sun_target_brightness` — целевая яркость после применения twilight-правил.
  // Это уже значение с учетом -10/-20/-30% и дневного выключения.
  append_meta(out, "candle_sun_target_brightness", "Target brightness after applying sun and twilight rules.", "gauge");
  append_uint_metric(out, "candle_sun_target_brightness", s_sunTargetBrightness.load(std::memory_order_relaxed));

  // `candle_sun_current_mode` — текущий режим по солнечному расписанию.
  // Коды: 0=unknown, 1=day, 2=civil, 3=nautical, 4=astronomical, 5=night, 6=disabled.
  append_meta(out, "candle_sun_current_mode", "Current sun mode code: 0=unknown, 1=day, 2=civil, 3=nautical, 4=astronomical, 5=night, 6=disabled.", "gauge");
  append_uint_metric(out, "candle_sun_current_mode", s_sunCurrentMode.load(std::memory_order_relaxed));

  // `candle_sun_control_mode` — источник управления яркостью: ручной или по солнцу.
  // Коды: 0=manual, 1=sun.
  append_meta(out, "candle_sun_control_mode", "Brightness control source: 0=manual, 1=sun.", "gauge");
  append_uint_metric(out, "candle_sun_control_mode", s_sunControlMode.load(std::memory_order_relaxed));

  // `candle_sun_current_mode_info` — текстовая метка текущего режима для удобства в Grafana/Prometheus.
  // Повторяет режим как label `mode` и numeric code в label `code`.
  append_meta(out, "candle_sun_current_mode_info", "Current sun mode exposed as labels.", "gauge");
  out += "candle_sun_current_mode_info{mode=\"";
  out += sun_mode_name(s_sunCurrentMode.load(std::memory_order_relaxed));
  out += "\",code=\"";
  out += String(s_sunCurrentMode.load(std::memory_order_relaxed));
  out += "\"} 1\n";

  // `candle_sun_control_mode_info` — текстовая метка источника управления яркостью.
  // Показывает `manual` или `sun` в label `mode`.
  append_meta(out, "candle_sun_control_mode_info", "Brightness control source exposed as labels.", "gauge");
  out += "candle_sun_control_mode_info{mode=\"";
  out += sun_control_mode_name(s_sunControlMode.load(std::memory_order_relaxed));
  out += "\",code=\"";
  out += String(s_sunControlMode.load(std::memory_order_relaxed));
  out += "\"} 1\n";

  // `candle_sun_attempts_today` — сколько попыток обновления `sun_position.json` уже было сегодня.
  // Сбрасывается при наступлении новых локальных суток.
  append_meta(out, "candle_sun_attempts_today", "How many refresh attempts for /sun_position.json were used today.", "gauge");
  append_uint_metric(out, "candle_sun_attempts_today", s_sunAttemptsToday.load(std::memory_order_relaxed));

  // `candle_sun_last_update_unixtime` — Unix time последнего успешного обновления файла `sun_position.json`.
  // Если значение старое, устройство работает по старому кэшу.
  append_meta(out, "candle_sun_last_update_unixtime", "Unix timestamp of the last successful /sun_position.json update.", "gauge");
  append_uint_metric(out, "candle_sun_last_update_unixtime", s_sunLastUpdateEpoch.load(std::memory_order_relaxed));

  // `candle_sun_*_minutes` — время астрособытий в минутах от начала локальных суток.
  // Удобно для Grafana и сравнения с текущим временем контроллера.
  append_meta(out, "candle_sun_sunrise_minutes", "Sunrise time as minutes from local midnight loaded from /sun_position.json.", "gauge");
  append_uint_metric(out, "candle_sun_sunrise_minutes", s_sunSunriseMinutes.load(std::memory_order_relaxed));
  append_meta(out, "candle_sun_sunset_minutes", "Sunset time as minutes from local midnight loaded from /sun_position.json.", "gauge");
  append_uint_metric(out, "candle_sun_sunset_minutes", s_sunSunsetMinutes.load(std::memory_order_relaxed));
  append_meta(out, "candle_sun_civil_twilight_begin_minutes", "Civil twilight begin as minutes from local midnight loaded from /sun_position.json.", "gauge");
  append_uint_metric(out, "candle_sun_civil_twilight_begin_minutes", s_sunCivilTwilightBeginMinutes.load(std::memory_order_relaxed));
  append_meta(out, "candle_sun_civil_twilight_end_minutes", "Civil twilight end as minutes from local midnight loaded from /sun_position.json.", "gauge");
  append_uint_metric(out, "candle_sun_civil_twilight_end_minutes", s_sunCivilTwilightEndMinutes.load(std::memory_order_relaxed));
  append_meta(out, "candle_sun_nautical_twilight_begin_minutes", "Nautical twilight begin as minutes from local midnight loaded from /sun_position.json.", "gauge");
  append_uint_metric(out, "candle_sun_nautical_twilight_begin_minutes", s_sunNauticalTwilightBeginMinutes.load(std::memory_order_relaxed));
  append_meta(out, "candle_sun_nautical_twilight_end_minutes", "Nautical twilight end as minutes from local midnight loaded from /sun_position.json.", "gauge");
  append_uint_metric(out, "candle_sun_nautical_twilight_end_minutes", s_sunNauticalTwilightEndMinutes.load(std::memory_order_relaxed));
  append_meta(out, "candle_sun_astronomical_twilight_begin_minutes", "Astronomical twilight begin as minutes from local midnight loaded from /sun_position.json.", "gauge");
  append_uint_metric(out, "candle_sun_astronomical_twilight_begin_minutes", s_sunAstronomicalTwilightBeginMinutes.load(std::memory_order_relaxed));
  append_meta(out, "candle_sun_astronomical_twilight_end_minutes", "Astronomical twilight end as minutes from local midnight loaded from /sun_position.json.", "gauge");
  append_uint_metric(out, "candle_sun_astronomical_twilight_end_minutes", s_sunAstronomicalTwilightEndMinutes.load(std::memory_order_relaxed));

  // `candle_i2c_clock_hz` — текущая частота шины I2C.
  // Может автоматически снижаться при ошибках связи с матрицей.
  append_meta(out, "candle_i2c_clock_hz", "Current I2C bus clock in Hz.", "gauge");
  append_uint_metric(out, "candle_i2c_clock_hz", s_i2cClockHz.load(std::memory_order_relaxed));

  // `candle_i2c_clock_fallback_active` — включен ли аварийный замедленный режим I2C.
  // 1 = шина переведена на fallback clock после серии ошибок, 0 = обычная скорость.
  append_meta(out, "candle_i2c_clock_fallback_active", "1 when the I2C bus is running in fallback slow-clock mode.", "gauge");
  append_uint_metric(out, "candle_i2c_clock_fallback_active", s_i2cClockFallbackActive.load(std::memory_order_relaxed));

  // `candle_i2c_current_page` — какая страница IS31FL3731 сейчас показана на матрице.
  // Помогает понять, зависло ли переключение double-buffer страниц.
  append_meta(out, "candle_i2c_current_page", "Currently displayed IS31FL3731 page.", "gauge");
  append_uint_metric(out, "candle_i2c_current_page", s_i2cCurrentPage.load(std::memory_order_relaxed));

  // `candle_i2c_consecutive_errors` — сколько неудачных попыток рендера идет подряд прямо сейчас.
  // Если растет, можно заранее увидеть деградацию шины или питания.
  append_meta(out, "candle_i2c_consecutive_errors", "Current consecutive failed frame pushes.", "gauge");
  append_uint_metric(out, "candle_i2c_consecutive_errors", s_i2cConsecutiveErrors.load(std::memory_order_relaxed));

  // `candle_i2c_consecutive_success` — длина текущей серии успешных или пропущенных без ошибки циклов.
  // Полезно для оценки, восстановилась ли стабильность после проблем.
  append_meta(out, "candle_i2c_consecutive_success", "Current consecutive successful or skipped animation cycles.", "gauge");
  append_uint_metric(out, "candle_i2c_consecutive_success", s_i2cConsecutiveSuccess.load(std::memory_order_relaxed));

  // `candle_i2c_max_consecutive_errors` — максимальная зафиксированная серия ошибок подряд.
  // Показывает, насколько тяжелыми были сбои за время работы устройства.
  append_meta(out, "candle_i2c_max_consecutive_errors", "Maximum observed consecutive failed frame pushes.", "gauge");
  append_uint_metric(out, "candle_i2c_max_consecutive_errors", s_i2cMaxConsecutiveErrors.load(std::memory_order_relaxed));

  // `candle_i2c_max_consecutive_success` — лучшая серия стабильной работы шины без сбоев.
  // Удобно как индикатор общей устойчивости анимационного цикла.
  append_meta(out, "candle_i2c_max_consecutive_success", "Maximum observed consecutive successful or skipped animation cycles.", "gauge");
  append_uint_metric(out, "candle_i2c_max_consecutive_success", s_i2cMaxConsecutiveSuccess.load(std::memory_order_relaxed));

  // `candle_i2c_last_frame_changed_bytes` — сколько байт PWM реально изменилось в последнем кадре.
  // Помогает отделить проблему шины от ситуации, когда анимация почти статична.
  append_meta(out, "candle_i2c_last_frame_changed_bytes", "How many PWM bytes changed in the latest frame diff.", "gauge");
  append_uint_metric(out, "candle_i2c_last_frame_changed_bytes", s_i2cLastFrameChangedBytes.load(std::memory_order_relaxed));

  // `candle_i2c_max_frame_changed_bytes` — максимальный объем изменений в одном кадре.
  // Чем выше значение, тем тяжелее потенциальная нагрузка на передачу.
  append_meta(out, "candle_i2c_max_frame_changed_bytes", "Maximum changed PWM bytes seen in one frame diff.", "gauge");
  append_uint_metric(out, "candle_i2c_max_frame_changed_bytes", s_i2cMaxFrameChangedBytes.load(std::memory_order_relaxed));

  // `candle_i2c_changed_bytes_total` — суммарное число измененных байт по всем кадрам.
  // Это интегральная оценка общей нагрузки анимации на I2C.
  append_meta(out, "candle_i2c_changed_bytes_total", "Total number of PWM bytes changed across all animation frame diffs.", "counter");
  append_uint_metric(out, "candle_i2c_changed_bytes_total", s_i2cChangedBytesTotal.load(std::memory_order_relaxed));

  // `candle_i2c_bus_bytes_written_total` — сколько полезных байт успешно прошло по шине.
  // При росте retry и ошибок помогает соотносить сбои с фактической нагрузкой.
  append_meta(out, "candle_i2c_bus_bytes_written_total", "Approximate number of payload bytes successfully written over the I2C bus.", "counter");
  append_uint_metric(out, "candle_i2c_bus_bytes_written_total", s_i2cBusBytesWrittenTotal.load(std::memory_order_relaxed));

  // `candle_i2c_last_render_duration_milliseconds` — сколько заняла последняя попытка записи кадра и page flip.
  // Рост времени часто указывает на retry, деградацию связи или переход на fallback speed.
  append_meta(out, "candle_i2c_last_render_duration_milliseconds", "Duration of the last attempted frame write plus page flip in milliseconds.", "gauge");
  append_float_metric(out, "candle_i2c_last_render_duration_milliseconds",
                      s_i2cLastRenderDurationUs.load(std::memory_order_relaxed) / 1000.0f, 3);

  // `candle_i2c_max_render_duration_milliseconds` — самый долгий цикл вывода кадра за все время работы.
  // Помогает ловить редкие зависания и пиковые задержки рендера.
  append_meta(out, "candle_i2c_max_render_duration_milliseconds", "Maximum observed frame write plus page flip duration in milliseconds.", "gauge");
  append_float_metric(out, "candle_i2c_max_render_duration_milliseconds",
                      s_i2cMaxRenderDurationUs.load(std::memory_order_relaxed) / 1000.0f, 3);

  // `candle_i2c_last_recovery_duration_milliseconds` — длительность последней процедуры восстановления I2C.
  // Важно для понимания, насколько заметной была просадка анимации во время recover.
  append_meta(out, "candle_i2c_last_recovery_duration_milliseconds", "Duration of the latest I2C recovery attempt in milliseconds.", "gauge");
  append_uint_metric(out, "candle_i2c_last_recovery_duration_milliseconds", s_i2cLastRecoveryDurationMs.load(std::memory_order_relaxed));

  // `candle_i2c_max_recovery_duration_milliseconds` — максимально долгий recovery с момента загрузки.
  // Если растет, стоит проверить питание, шлейфы и сам драйвер матрицы.
  append_meta(out, "candle_i2c_max_recovery_duration_milliseconds", "Maximum observed I2C recovery duration in milliseconds.", "gauge");
  append_uint_metric(out, "candle_i2c_max_recovery_duration_milliseconds", s_i2cMaxRecoveryDurationMs.load(std::memory_order_relaxed));

  // `candle_i2c_last_error_code` — последний ненулевой код ошибки от `Wire.endTransmission()`.
  // Позволяет быстро понять тип последнего аппаратного сбоя на шине.
  append_meta(out, "candle_i2c_last_error_code", "Last non-zero Wire.endTransmission error code, or 0 when none was recorded.", "gauge");
  append_int_metric(out, "candle_i2c_last_error_code", lastErrorCode);

  // `candle_i2c_last_error_unixtime` — Unix time последней низкоуровневой ошибки I2C.
  // Удобно для сопоставления с логами, OTA и внешними событиями.
  append_meta(out, "candle_i2c_last_error_unixtime", "Unix timestamp of the last low-level I2C bus error.", "gauge");
  append_uint_metric(out, "candle_i2c_last_error_unixtime", s_i2cLastErrorEpoch.load(std::memory_order_relaxed));

  // `candle_i2c_last_success_unixtime` — время последнего успешного рендера, skip без ошибки или recovery.
  // Если давно не обновляется, а устройство живо — значит есть проблема в анимационном контуре.
  append_meta(out, "candle_i2c_last_success_unixtime", "Unix timestamp of the last successful render, skipped cycle or bus recovery.", "gauge");
  append_uint_metric(out, "candle_i2c_last_success_unixtime", s_i2cLastSuccessEpoch.load(std::memory_order_relaxed));

  // `candle_i2c_last_recovery_unixtime` — когда в последний раз запускалось восстановление шины.
  // Помогает видеть, насколько недавно устройство боролось с I2C-сбоем.
  append_meta(out, "candle_i2c_last_recovery_unixtime", "Unix timestamp of the latest I2C recovery attempt.", "gauge");
  append_uint_metric(out, "candle_i2c_last_recovery_unixtime", s_i2cLastRecoveryEpoch.load(std::memory_order_relaxed));

  // `candle_device_info` — служебная метрика с label'ами устройства.
  // Через labels отдает `devname`, читаемое имя устройства и текущий IP-адрес.
  append_meta(out, "candle_device_info", "Device metadata exposed as labels.", "gauge");
  out += "candle_device_info{devname=\"";
  out += escape_prometheus_label(String(cfg.wifi.devname));
  out += "\",name=\"";
  out += escape_prometheus_label(String(cfg.wifi.name));
  out += "\",ip=\"";
  out += escape_prometheus_label(ip);
  out += "\"} 1\n";

  // `candle_i2c_last_error_info` — текстовая расшифровка последней ошибки I2C.
  // В labels хранит stage выполнения и имя кода ошибки для быстрой диагностики в Grafana/Prometheus.
  append_meta(out, "candle_i2c_last_error_info", "Metadata about the latest low-level I2C bus error.", "gauge");
  out += "candle_i2c_last_error_info{stage=\"";
  out += i2c_stage_name(lastErrorStage);
  out += "\",code=\"";
  if (lastErrorCode >= 0) {
    out += String(lastErrorCode);
  } else {
    out += "unknown";
  }
  out += "\",name=\"";
  out += i2c_error_code_name(lastErrorCode);
  out += "\"} ";
  out += String(lastErrorCode == 0 ? 0 : 1);
  out += '\n';

  // `candle_wifi_connect_attempts_total` — сколько раз устройство пыталось подключиться к Wi‑Fi.
  // Счетчик увеличивается при каждом новом вызове `WiFi.begin()`.
  append_meta(out, "candle_wifi_connect_attempts_total", "Total Wi-Fi connection attempts.", "counter");
  append_uint_metric(out, "candle_wifi_connect_attempts_total", s_wifiConnectAttemptsTotal.load(std::memory_order_relaxed));

  // `candle_wifi_connect_success_total` — число успешных подключений к Wi‑Fi.
  // Растет при каждом переходе устройства в состояние `WL_CONNECTED`.
  append_meta(out, "candle_wifi_connect_success_total", "Total successful Wi-Fi connections.", "counter");
  append_uint_metric(out, "candle_wifi_connect_success_total", s_wifiConnectedTotal.load(std::memory_order_relaxed));

  // `candle_wifi_disconnects_total` — сколько раз связь по Wi‑Fi терялась после успешного подключения.
  // Удобно для контроля стабильности сети.
  append_meta(out, "candle_wifi_disconnects_total", "Total Wi-Fi disconnect events.", "counter");
  append_uint_metric(out, "candle_wifi_disconnects_total", s_wifiDisconnectsTotal.load(std::memory_order_relaxed));

  // `candle_http_requests_total` — число запросов к динамическим HTTP endpoint'ам.
  // Сейчас учитываются обращения к `/`, `/api/brightness` и `/metrics`.
  append_meta(out, "candle_http_requests_total", "HTTP requests handled by dynamic endpoints.", "counter");
  append_uint_metric(out, "candle_http_requests_total", s_httpRequestsTotal.load(std::memory_order_relaxed));

  // `candle_prometheus_scrapes_total` — сколько раз Prometheus или другой клиент прочитал `/metrics`.
  // Это помогает понять частоту опроса устройства.
  append_meta(out, "candle_prometheus_scrapes_total", "How many times /metrics was scraped.", "counter");
  append_uint_metric(out, "candle_prometheus_scrapes_total", s_prometheusScrapesTotal.load(std::memory_order_relaxed));

  // `candle_ntp_sync_failures_total` — сколько попыток синхронизации времени завершились неудачей.
  // При росте счетчика нужно проверить доступность NTP-сервера и качество сети.
  append_meta(out, "candle_ntp_sync_failures_total", "Failed NTP synchronization attempts.", "counter");
  append_uint_metric(out, "candle_ntp_sync_failures_total", s_ntpFailuresTotal.load(std::memory_order_relaxed));

  // `candle_sun_refresh_requests_total` — сколько раз модуль планировал обновление `sun_position.json`.
  // Это число может быть больше числа удачных обновлений, если были сетевые проблемы.
  append_meta(out, "candle_sun_refresh_requests_total", "How many times the sunrise/sunset cache refresh was scheduled.", "counter");
  append_uint_metric(out, "candle_sun_refresh_requests_total", s_sunRefreshRequestsTotal.load(std::memory_order_relaxed));

  // `candle_sun_update_success_total` — число успешных обновлений файла `sun_position.json`.
  // Растет только после записи файла и последующей успешной загрузки расписания из него.
  append_meta(out, "candle_sun_update_success_total", "Successful refreshes of /sun_position.json that were reloaded into runtime state.", "counter");
  append_uint_metric(out, "candle_sun_update_success_total", s_sunUpdateSuccessTotal.load(std::memory_order_relaxed));

  // `candle_sun_update_failures_total` — число неудачных попыток обновить `sun_position.json`.
  // Если растет, устройство переходит на последние сохраненные в LittleFS значения.
  append_meta(out, "candle_sun_update_failures_total", "Failed refresh attempts for /sun_position.json.", "counter");
  append_uint_metric(out, "candle_sun_update_failures_total", s_sunUpdateFailuresTotal.load(std::memory_order_relaxed));

  // `candle_i2c_frames_rendered_total` — сколько кадров анимации реально отправлено в LED-драйвер.
  // Растет только после успешной записи кадра и переключения страницы.
  append_meta(out, "candle_i2c_frames_rendered_total", "Frames written to the LED driver.", "counter");
  append_uint_metric(out, "candle_i2c_frames_rendered_total", s_i2cFramesRenderedTotal.load(std::memory_order_relaxed));

  // `candle_i2c_frames_skipped_total` — сколько кадров пропущено, потому что изображение не изменилось.
  // Это снижает нагрузку на I2C и уменьшает вероятность зависаний.
  append_meta(out, "candle_i2c_frames_skipped_total", "Frames skipped because image data was unchanged.", "counter");
  append_uint_metric(out, "candle_i2c_frames_skipped_total", s_i2cFramesSkippedTotal.load(std::memory_order_relaxed));

  // `candle_i2c_errors_total` — сколько раз цикл вывода кадра завершился ошибкой.
  // Это основной счетчик проблем связи с драйвером IS31FL3731.
  append_meta(out, "candle_i2c_errors_total", "Animation frame pushes that failed and triggered error handling.", "counter");
  append_uint_metric(out, "candle_i2c_errors_total", s_i2cErrorsTotal.load(std::memory_order_relaxed));

  // `candle_i2c_recoveries_total` — число успешных попыток восстановления I2C после ошибок.
  // Показывает, как часто устройству приходилось "лечить" шину.
  append_meta(out, "candle_i2c_recoveries_total", "Successful I2C bus recovery attempts.", "counter");
  append_uint_metric(out, "candle_i2c_recoveries_total", s_i2cRecoveriesTotal.load(std::memory_order_relaxed));

  // `candle_i2c_recovery_failures_total` — число неудачных восстановлений I2C.
  // Если растет, стоит проверять питание, провода и сам дисплейный драйвер.
  append_meta(out, "candle_i2c_recovery_failures_total", "Failed I2C bus recovery attempts.", "counter");
  append_uint_metric(out, "candle_i2c_recovery_failures_total", s_i2cRecoveryFailuresTotal.load(std::memory_order_relaxed));

  // `candle_i2c_clock_fallbacks_total` — сколько раз частота шины была снижена до fallback-режима.
  // Важный индикатор повторяющихся проблем связи под нагрузкой.
  append_meta(out, "candle_i2c_clock_fallbacks_total", "How many times the bus clock dropped into fallback mode after repeated errors.", "counter");
  append_uint_metric(out, "candle_i2c_clock_fallbacks_total", s_i2cClockFallbacksTotal.load(std::memory_order_relaxed));

  // `candle_i2c_clock_restores_total` — сколько раз шина вернулась на штатную скорость после стабилизации.
  // Позволяет увидеть, происходят ли самовосстановления после деградации.
  append_meta(out, "candle_i2c_clock_restores_total", "How many times the bus clock returned to the default speed after stability recovered.", "counter");
  append_uint_metric(out, "candle_i2c_clock_restores_total", s_i2cClockRestoresTotal.load(std::memory_order_relaxed));

  // `candle_i2c_stage_attempts_total` — число попыток операций по стадиям I2C-обмена.
  // Label `stage` показывает, где именно проходит нагрузка: выбор страницы, запись чанка, flip и т.д.
  append_meta(out, "candle_i2c_stage_attempts_total", "I2C operation attempts grouped by stage.", "counter");
  append_i2c_stage_metric(out, "candle_i2c_stage_attempts_total", s_i2cStageAttemptsTotal);

  // `candle_i2c_stage_success_total` — успешные выполнения операций по каждой стадии.
  // Позволяет сравнить объем удачных операций с количеством попыток и ошибок.
  append_meta(out, "candle_i2c_stage_success_total", "Successful I2C operations grouped by stage.", "counter");
  append_i2c_stage_metric(out, "candle_i2c_stage_success_total", s_i2cStageSuccessTotal);

  // `candle_i2c_stage_failures_total` — количество провалов по стадиям.
  // Это одна из самых полезных метрик для локализации, где именно ломается обмен.
  append_meta(out, "candle_i2c_stage_failures_total", "Failed I2C operations grouped by stage.", "counter");
  append_i2c_stage_metric(out, "candle_i2c_stage_failures_total", s_i2cStageFailuresTotal);

  // `candle_i2c_stage_retries_total` — сколько дополнительных повторов потребовала каждая стадия.
  // Если растет раньше общего счетчика ошибок, это ранний симптом начинающейся деградации шины.
  append_meta(out, "candle_i2c_stage_retries_total", "How many retry attempts were used for each I2C stage.", "counter");
  append_i2c_stage_metric(out, "candle_i2c_stage_retries_total", s_i2cStageRetriesTotal);

  // `candle_i2c_stage_bytes_written_total` — сколько байт было успешно записано на каждой стадии.
  // Полезно для сопоставления проблем с объемом передаваемых данных.
  append_meta(out, "candle_i2c_stage_bytes_written_total", "Payload bytes written per I2C stage.", "counter");
  append_i2c_stage_metric(out, "candle_i2c_stage_bytes_written_total", s_i2cStageBytesWrittenTotal);

  // `candle_i2c_end_transmission_errors_total` — распределение кодов ошибок `Wire.endTransmission()`.
  // Позволяет отличить `address_nack`, `data_nack`, timeout и другие типы аппаратных сбоев.
  append_meta(out, "candle_i2c_end_transmission_errors_total", "Wire.endTransmission non-zero result codes grouped by error code.", "counter");
  append_i2c_error_metric(out, "candle_i2c_end_transmission_errors_total", s_i2cEndTransmissionErrorTotal);

  return out;
}
