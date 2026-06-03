#pragma once

#include <Arduino.h>
#include <stdint.h>

enum class I2cMetricStage : uint8_t {
  Init = 0,
  SelectPage,
  WriteChunk,
  WriteFrame,
  FlipPage,
  Recover,
  Count
};

void metrics_record_wifi_connect_attempt();
void metrics_record_wifi_connected();
void metrics_record_wifi_disconnect();

void metrics_record_http_request();
void metrics_record_prometheus_scrape();

void metrics_record_ntp_sync(int32_t driftSeconds, uint32_t syncedAtEpoch);
void metrics_record_ntp_failure();

void metrics_record_i2c_frame_rendered();
void metrics_record_i2c_frame_skipped();
void metrics_record_i2c_error();
void metrics_record_i2c_stage_attempt(I2cMetricStage stage);
void metrics_record_i2c_stage_success(I2cMetricStage stage, uint16_t bytesWritten = 0, uint8_t retriesUsed = 0);
void metrics_record_i2c_stage_failure(I2cMetricStage stage, uint8_t retriesUsed = 0);
void metrics_record_i2c_bus_error(I2cMetricStage stage, int errorCode);
void metrics_record_i2c_recover(bool success, uint32_t durationMs);
void metrics_set_i2c_state(uint8_t currentPage, uint32_t consecutiveErrors, uint32_t consecutiveSuccess);
void metrics_record_i2c_frame_change(uint16_t changedBytes);
void metrics_record_i2c_render_duration(uint32_t durationUs);
void metrics_record_i2c_clock_fallback(bool active);

void metrics_set_i2c_clock_hz(uint32_t hz);
void metrics_set_brightness(uint8_t value);

void metrics_set_sun_state(bool cacheLoaded, bool candleEnabled, bool fetchInProgress, uint8_t targetBrightness, uint8_t attemptsToday);
void metrics_set_sun_mode(uint8_t currentMode);
void metrics_set_sun_control_mode(bool sunControlled);
void metrics_set_sun_schedule(uint32_t updatedAtEpoch,
                              uint16_t sunriseMinutes,
                              uint16_t sunsetMinutes,
                              uint16_t civilTwilightBeginMinutes,
                              uint16_t civilTwilightEndMinutes,
                              uint16_t nauticalTwilightBeginMinutes,
                              uint16_t nauticalTwilightEndMinutes,
                              uint16_t astronomicalTwilightBeginMinutes,
                              uint16_t astronomicalTwilightEndMinutes);
void metrics_record_sun_refresh_request();
void metrics_record_sun_update_success();
void metrics_record_sun_update_failure();
void metrics_record_sun_no_schedule_forced_off();

String metrics_render_prometheus();
