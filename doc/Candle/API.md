# HTTP API

The device serves the web UI and HTTP API on port `80`. JSON API responses use `application/json`. Dynamic responses that represent live state use `Cache-Control: no-store`.

The API is designed for a trusted local network and does not include authentication.

## Endpoint Summary

| Method | Path                     | Description                                                                           |
| ------ | ------------------------ | ------------------------------------------------------------------------------------- |
| `GET`  | `/`                      | Web UI entry point from LittleFS.                                                     |
| `GET`  | `/api/settings`          | Current application settings, firmware metadata, Wi-Fi display state, and mode flags. |
| `POST` | `/api/save`              | Save settings and restart.                                                            |
| `GET`  | `/api/date`              | Current local time, NTP diagnostics, Wi-Fi status, and candle state.                  |
| `POST` | `/api/time`              | Set manual local time before NTP synchronization.                                     |
| `GET`  | `/api/sun`               | Current sun position and local-day solar events.                                      |
| `GET`  | `/api/moon`              | Current moon phase and moon LED state.                                                |
| `POST` | `/api/moon-led`          | Save and apply optional WS2812 moon LED settings without restart.                     |
| `GET`  | `/api/brightness`        | Current configured brightness.                                                        |
| `POST` | `/api/brightness`        | Update brightness and optional manual/sun/time-schedule flags.                        |
| `POST` | `/api/reset`             | Clear Wi-Fi credentials and restart.                                                  |
| `POST` | `/api/update/firmware`   | Upload `firmware.bin` as raw binary.                                                  |
| `POST` | `/api/update/filesystem` | Upload `littlefs.bin` as raw binary.                                                  |
| `POST` | `/api/update/restart`    | Restart after web update.                                                             |
| `GET`  | `/api/update`            | OTA status.                                                                           |
| `GET`  | `/metrics`               | Prometheus metrics.                                                                   |

Captive portal probe paths such as `/generate_204`, `/gen_204`, `/hotspot-detect.html`, `/ncsi.txt`, `/connecttest.txt`, and `/fwlink` are redirected to the setup portal while captive portal mode is active.

## Common Status Response

Error responses use this shape:

```json
{
  "status": "error",
  "message": "invalid JSON payload"
}
```

Successful command responses use:

```json
{
  "status": "ok",
  "message": "device restarting"
}
```

## GET `/api/settings`

Returns current settings plus UI metadata.

Example:

```json
{
  "devname": "candle-light1",
  "name": "Candle",
  "firmwareVersion": "1.5.3",
  "buildCommit": "a1b2c3d4e5f6",
  "buildDate": "2026-06-25T09:30:00Z",
  "ssid": "HomeWiFi",
  "password": "",
  "ntp_server": "pool.ntp.org",
  "ntp_server2": "",
  "ntp_timezone": "Europe/Moscow",
  "validTime": true,
  "ntpSynchronized": true,
  "manualTimeAllowed": false,
  "wifiStaConnected": true,
  "wifiHasIp": true,
  "lat": 55.4997807,
  "lon": 35.9809486,
  "autoMode": true,
  "autoTimeMode": false,
  "timeOnMinute": 1080,
  "timeOffMinute": 1380,
  "sunMode": "night",
  "autoCandleOn": true,
  "autoTimeCandleOn": false,
  "brightness": 16,
  "candleOn": true,
  "moonLed": {
    "enabled": false,
    "maxBrightness": 25,
    "hue": 42,
    "currentBrightness": 0,
    "hardwareEnabled": true
  }
}
```

| Field                       | Type    | Description                                                                                                        |
| --------------------------- | ------- | ------------------------------------------------------------------------------------------------------------------ |
| `devname`                   | string  | Device hostname.                                                                                                   |
| `name`                      | string  | Display name.                                                                                                      |
| `firmwareVersion`           | string  | Firmware version from `FIRMWARE_VERSION`.                                                                          |
| `buildCommit`               | string  | Git commit embedded at build time, or `unknown` when unavailable.                                                  |
| `buildDate`                 | string  | CI commit timestamp, source-date timestamp, git commit date, or `unknown`.                                         |
| `ssid`                      | string  | Stored Wi-Fi SSID, if configured.                                                                                  |
| `password`                  | string  | Always empty. Passwords are not returned.                                                                          |
| `ntp_server`                | string  | Primary NTP server.                                                                                                |
| `ntp_server2`               | string  | Optional secondary NTP server.                                                                                     |
| `ntp_timezone`              | string  | Supported timezone name or POSIX timezone string.                                                                  |
| `validTime`                 | bool    | Current time is later than the firmware validity threshold.                                                        |
| `ntpSynchronized`           | bool    | Time has been synchronized by NTP.                                                                                 |
| `manualTimeAllowed`         | bool    | Manual time can still be set through `/api/time`.                                                                  |
| `wifiStaConnected`          | bool    | Station interface is connected to Wi-Fi.                                                                           |
| `wifiHasIp`                 | bool    | Station interface has an IP address.                                                                               |
| `lat`                       | number  | Latitude.                                                                                                          |
| `lon`                       | number  | Longitude.                                                                                                         |
| `autoMode`                  | bool    | Sun-based automatic mode enabled.                                                                                  |
| `autoTimeMode`              | bool    | Fixed daily time schedule mode enabled.                                                                            |
| `timeOnMinute`              | integer | Schedule start minute from local midnight, `0..1439`.                                                              |
| `timeOffMinute`             | integer | Schedule stop minute from local midnight, `0..1439`.                                                               |
| `sunMode`                   | string  | Current mode: `day`, `civil`, `nautical`, `astronomical`, `night`, `time`, `manual`, `waiting_time`, or `unknown`. |
| `autoCandleOn`              | bool    | Output requested by the active automatic mode.                                                                     |
| `autoTimeCandleOn`          | bool    | Output requested specifically by fixed time schedule mode.                                                         |
| `brightness`                | integer | Configured brightness percent, `0..100`.                                                                           |
| `candleOn`                  | bool    | Manual output state.                                                                                               |
| `moonLed.enabled`           | bool    | Optional moon WS2812 LED enabled.                                                                                  |
| `moonLed.maxBrightness`     | integer | Upper moon LED brightness limit, `0..100`.                                                                         |
| `moonLed.hue`               | integer | Moon LED hue in degrees, `0..360`.                                                                                 |
| `moonLed.currentBrightness` | integer | Current calculated moon LED output brightness, `0..100`.                                                           |
| `moonLed.hardwareEnabled`   | bool    | `true` when `MOON_LED_PIN >= 0` at build time.                                                                     |

## POST `/api/save`

Saves application settings and restarts the device.

Request body is JSON. All fields are optional; omitted fields keep their current value.

```json
{
  "devname": "candle-light1",
  "name": "Candle",
  "ssid": "HomeWiFi",
  "password": "secret",
  "ntp_server": "pool.ntp.org",
  "ntp_server2": "",
  "ntp_timezone": "Europe/Moscow",
  "lat": 55.4997807,
  "lon": 35.9809486,
  "autoMode": true,
  "autoTimeMode": false,
  "timeOnMinute": 1080,
  "timeOffMinute": 1380,
  "brightness": 50,
  "candleOn": true,
  "moonLed": {
    "enabled": true,
    "maxBrightness": 25,
    "hue": 42
  }
}
```

Behavior:

- JSON body size is limited to 4096 bytes.
- `brightness` accepts `0..100`; values `101..255` are converted to percent.
- `autoMode` enables sun-based mode.
- `autoTimeMode` enables fixed time schedule mode.
- Sun mode and fixed time schedule mode are mutually exclusive; when `autoTimeMode` is true, `autoMode` is cleared by runtime normalization.
- `timeOnMinute` and `timeOffMinute` accept `0..1439`.
- `candleOn` is used only when manual control is effective: neither automatic mode is active, or valid time is unavailable.
- `brightness = 0` forces `candleOn = false`.
- `moonLed.maxBrightness` accepts `0..100`; values `101..255` are converted to percent.
- `moonLed.hue` accepts `0..360`.
- Wi-Fi credentials are stored in NVS, not in `settings.json`.
- If `password` is empty and the SSID did not change, the stored Wi-Fi password is kept.

Successful response:

```json
{
  "status": "ok",
  "restarting": true,
  "message": "settings saved, device restarting"
}
```

Possible errors:

| HTTP  | Message                            |
| ----- | ---------------------------------- |
| `400` | `invalid JSON payload`             |
| `413` | `settings payload is too large`    |
| `500` | `request buffer allocation failed` |
| `500` | `failed to save settings`          |
| `500` | `failed to save Wi-Fi credentials` |

## GET `/api/date`

Returns current local time, candle state, NTP diagnostics, and Wi-Fi diagnostics.

Example:

```json
{
  "valid": true,
  "date": "14:35 28.06.2026",
  "sunMode": "night",
  "autoCandleOn": true,
  "autoTimeCandleOn": false,
  "candleOn": true,
  "ntp": {
    "wifiConnected": true,
    "hasIp": true,
    "syncInProgress": false,
    "validTime": true,
    "ntpSynchronized": true,
    "manualTimeAllowed": false,
    "bootSyncPending": false,
    "dnsResolved": true,
    "failureStreak": 0,
    "sntpSyncStatus": 1,
    "nextSyncInMs": 21420000,
    "syncTimeoutInMs": 0,
    "lastSuccessEpoch": 1782657300,
    "server": "pool.ntp.org",
    "serverIp": "162.159.200.1",
    "timezone": "Europe/Moscow"
  },
  "wifi": {
    "staConnected": true,
    "setupApActive": false,
    "ssid": "HomeWiFi",
    "ip": "192.168.1.45",
    "rssi": -62,
    "setupSsid": "candle-setup",
    "setupIp": ""
  }
}
```

NTP status notes:

| Field                 | Description                                                            |
| --------------------- | ---------------------------------------------------------------------- |
| `valid` / `validTime` | `true` when system time is later than the firmware validity threshold. |
| `ntpSynchronized`     | `true` after successful NTP synchronization.                           |
| `manualTimeAllowed`   | `true` when `/api/time` can still set time manually.                   |
| `sntpSyncStatus`      | ESP SNTP status: `0` reset/waiting, `1` completed, `2` in progress.    |
| `dnsResolved`         | Primary NTP hostname resolved before sync.                             |
| `failureStreak`       | Consecutive NTP failures.                                              |
| `nextSyncInMs`        | Delay until the next scheduled sync attempt.                           |
| `syncTimeoutInMs`     | Remaining timeout for the active sync attempt.                         |

## POST `/api/time`

Sets manual local time before NTP synchronization. The endpoint accepts either:

- query or form parameter `value`;
- a JSON body with `{"value":"HH:MM DD.MM.YYYY"}`;
- a plain small body containing `HH:MM DD.MM.YYYY`.

Example:

```http
POST /api/time?value=15:19%2019.06.2026
```

Successful response contains the same live fields as `/api/date`, plus top-level time flags:

```json
{
  "status": "ok",
  "validTime": true,
  "ntpSynchronized": false,
  "manualTimeAllowed": true,
  "wifiStaConnected": true,
  "wifiHasIp": true
}
```

Possible errors:

| HTTP  | Message                                                    |
| ----- | ---------------------------------------------------------- |
| `400` | `time value is required`                                   |
| `400` | `time format must be HH:MM DD.MM.YYYY`                     |
| `409` | `time is already synchronized by NTP`                      |
| `409` | `manual time is available only before NTP synchronization` |
| `413` | `time payload is too large`                                |
| `500` | `request buffer allocation failed`                         |
| `500` | `failed to set manual time`                                |

## GET `/api/sun`

Calculates current sun position and local-day events. The endpoint uses configured coordinates unless query parameters override them.

Query parameters:

| Parameter | Description             |
| --------- | ----------------------- |
| `lat`     | Latitude, `-90..90`.    |
| `lon`     | Longitude, `-180..180`. |
| `lng`     | Alias for `lon`.        |

Example:

```json
{
  "status": "ok",
  "validTime": true,
  "timestamp": 1782657300,
  "timezoneOffsetMinutes": 180,
  "sunMode": "day",
  "pathSampleMinutes": 10,
  "pathAlgorithm": "raw-10m-threshold-crossings-v3",
  "pathMinElevation": -12.5,
  "pathMaxElevation": 57.2,
  "location": { "lat": 55.4997807, "lon": 35.9809486 },
  "date": { "year": 2026, "month": 6, "day": 28 },
  "now": {
    "minuteOfDay": 875,
    "azimuth": 248.3,
    "elevation": 32.1,
    "zenith": 57.9,
    "declination": 21.5,
    "equationOfTime": 2.8
  },
  "events": {
    "hasSunrise": true,
    "hasSunset": true,
    "hasCivilTwilight": true,
    "hasNauticalTwilight": true,
    "hasAstronomicalTwilight": true,
    "isPolarDay": false,
    "isPolarNight": false,
    "sunrise": 238,
    "sunset": 1213,
    "civilBegin": 205,
    "civilEnd": 1246,
    "nauticalBegin": 164,
    "nauticalEnd": 1287,
    "astronomicalBegin": 120,
    "astronomicalEnd": 1331
  },
  "path": [
    { "minute": 0, "azimuth": 2.1, "elevation": -23.4, "mode": "night" },
    { "minute": 10, "azimuth": 4.8, "elevation": -22.1, "mode": "night" }
  ]
}
```

Event time fields are minutes from local midnight in the range `0..1439`.

`path` is built from 10-minute daily samples plus exact threshold crossing points for `0`, `-6`, `-12`, and `-18` degrees. The response includes `pathSampleMinutes`, `pathAlgorithm`, `pathMinElevation`, and `pathMaxElevation`.

Possible errors:

| HTTP  | Message                                                                 |
| ----- | ----------------------------------------------------------------------- |
| `400` | `invalid lat`, `invalid lon`, `invalid lng`, `coordinates out of range` |
| `500` | `failed to resolve local time`                                          |
| `500` | `failed to calculate current sun position`                              |
| `500` | `failed to calculate sun events`                                        |
| `500` | `failed to calculate sun path`                                          |
| `503` | `insufficient memory for sun payload`                                   |

## GET `/api/moon`

Returns the current moon phase and moon LED state.

Example:

```json
{
  "status": "ok",
  "validTime": true,
  "timestamp": 1782657300,
  "ageDays": 12.8,
  "illumination": 0.91,
  "phase": 3,
  "phaseName": "Waxing gibbous",
  "moonLed": {
    "enabled": true,
    "maxBrightness": 25,
    "hue": 42,
    "currentBrightness": 23,
    "hardwareEnabled": true
  }
}
```

If time is not available, the endpoint returns HTTP `503`:

```json
{ "status": "error", "message": "time not available" }
```

## POST `/api/moon-led`

Saves optional WS2812 moon LED settings and applies them immediately without restart. Parameters may be query string or form fields.

| Parameter               | Required | Description                                                             |
| ----------------------- | -------- | ----------------------------------------------------------------------- |
| `enabled`               | no       | `0`, `1`, `true`, `false`, `on`, or `off`.                              |
| `value` or `brightness` | no       | Maximum moon LED brightness, `0..100`. Values `101..255` are converted. |
| `hue`                   | no       | Hue in degrees, `0..360`.                                               |

Successful response:

```json
{
  "status": "ok",
  "moonLed": {
    "enabled": true,
    "maxBrightness": 25,
    "hue": 42,
    "currentBrightness": 23,
    "hardwareEnabled": true
  }
}
```

Possible errors:

| HTTP  | Message                                |
| ----- | -------------------------------------- |
| `400` | `hue must be an integer from 0 to 360` |
| `500` | `failed to save moon led settings`     |

The physical WS2812 output is available only when the firmware is built with `MOON_LED_PIN >= 0`.

## GET `/api/brightness`

Returns current configured brightness.

```json
{
  "status": "ok",
  "brightness": 50
}
```

## POST `/api/brightness`

Updates brightness and optional mode fields. Parameters may be query string or form fields.

| Parameter               | Required | Description                                                                |
| ----------------------- | -------- | -------------------------------------------------------------------------- |
| `value` or `brightness` | yes      | Brightness percent, `0..100`. Values `101..255` are converted.             |
| `autoMode`              | no       | Sun mode flag. Accepted only when valid time is available.                 |
| `autoTimeMode`          | no       | Fixed time schedule mode flag. Accepted only when valid time is available. |
| `timeOnMinute`          | no       | Schedule start minute from local midnight, `0..1439`.                      |
| `timeOffMinute`         | no       | Schedule stop minute from local midnight, `0..1439`.                       |
| `candleOn`              | no       | Manual output state when manual control is effective.                      |

Successful response:

```json
{
  "status": "ok",
  "brightness": 50,
  "candleOn": true,
  "autoMode": false,
  "autoTimeMode": true,
  "autoCandleOn": true,
  "autoTimeCandleOn": true,
  "sunMode": "time"
}
```

Errors:

| HTTP  | Message                                               |
| ----- | ----------------------------------------------------- |
| `400` | `brightness must be an integer percent from 0 to 100` |
| `500` | `failed to save brightness`                           |

## POST `/api/reset`

Clears Wi-Fi credentials, keeps application settings, and restarts the device.

```json
{
  "status": "ok",
  "message": "Wi-Fi credentials cleared, device restarting"
}
```

If saving the resulting settings state fails, the endpoint returns HTTP `500` with:

```json
{ "status": "error", "message": "failed to clear Wi-Fi credentials" }
```

## Web Update API

See [web_update.md](web_update.md) for full details.

### POST `/api/update/firmware`

Uploads `firmware.bin` as raw binary:

```http
POST /api/update/firmware
Content-Type: application/octet-stream
X-Update-Filename: firmware.bin
```

### POST `/api/update/filesystem`

Uploads `littlefs.bin` as raw binary:

```http
POST /api/update/filesystem
Content-Type: application/octet-stream
X-Update-Filename: littlefs.bin
```

### POST `/api/update/restart`

Schedules restart after firmware and filesystem uploads:

```json
{
  "status": "ok",
  "restarting": true,
  "restartScheduled": true,
  "restartInMs": 1500,
  "message": "device restarting"
}
```

### GET `/api/update`

Returns update status:

```json
{
  "status": "ok",
  "active": false,
  "error": "",
  "target": "filesystem",
  "maxSize": 1507328,
  "restartPending": false,
  "restartInMs": 0
}
```

## GET `/metrics`

Returns Prometheus text exposition using content type:

```text
text/plain; version=0.0.4; charset=utf-8
```

Important metric groups:

| Metric                                     | Type    | Description                                                                                                      |
| ------------------------------------------ | ------- | ---------------------------------------------------------------------------------------------------------------- |
| `candle_up`                                | gauge   | `1` while firmware is running.                                                                                   |
| `candle_device_info`                       | gauge   | Device metadata labels including hostname, firmware version, build commit, build date, and IP.                   |
| `candle_uptime_seconds`                    | gauge   | Seconds since boot.                                                                                              |
| `candle_heap_free_bytes`                   | gauge   | Currently free heap.                                                                                             |
| `candle_heap_min_free_bytes`               | gauge   | Minimum free heap seen since boot.                                                                               |
| `candle_current_unixtime`                  | gauge   | Current Unix timestamp.                                                                                          |
| `candle_wifi_connected`                    | gauge   | Wi-Fi state.                                                                                                     |
| `candle_wifi_rssi_dbm`                     | gauge   | Wi-Fi RSSI or `-127` when disconnected.                                                                          |
| `candle_wifi_connect_attempts_total`       | counter | Wi-Fi connection attempts.                                                                                       |
| `candle_wifi_connect_success_total`        | counter | Successful Wi-Fi connections.                                                                                    |
| `candle_wifi_disconnects_total`            | counter | Wi-Fi disconnect events.                                                                                         |
| `candle_http_requests_total`               | counter | Dynamic HTTP requests handled.                                                                                   |
| `candle_prometheus_scrapes_total`          | counter | `/metrics` scrapes.                                                                                              |
| `candle_ntp_drift_seconds`                 | gauge   | Clock correction on last successful NTP sync.                                                                    |
| `candle_ntp_last_success_unixtime`         | gauge   | Last successful NTP sync timestamp.                                                                              |
| `candle_ntp_sync_failures_total`           | counter | Failed NTP sync attempts.                                                                                        |
| `candle_ota_active`                        | gauge   | OTA upload/write in progress.                                                                                    |
| `candle_ota_success_total`                 | counter | Successful OTA uploads.                                                                                          |
| `candle_ota_failures_total`                | counter | Failed OTA uploads.                                                                                              |
| `candle_i2c_brightness`                    | gauge   | Current LED brightness in range `0..255`.                                                                        |
| `candle_sun_control_mode`                  | gauge   | Brightness control source: `0=manual`, `1=sun`.                                                                  |
| `candle_sun_current_mode`                  | gauge   | Current sun mode code: `0=unknown`, `1=day`, `2=civil`, `3=nautical`, `4=astronomical`, `5=night`, `6=disabled`. |
| `candle_sun_current_mode_info`             | gauge   | Current sun mode as labels.                                                                                      |
| `candle_sun_target_brightness`             | gauge   | Brightness requested by sun/time/manual control.                                                                 |
| `candle_sun_*_minutes`                     | gauge   | Sunrise, sunset, and twilight minutes from local midnight.                                                       |
| `candle_sun_refresh_requests_total`        | counter | Solar schedule recalculation requests.                                                                           |
| `candle_sun_update_success_total`          | counter | Successful solar schedule recalculations.                                                                        |
| `candle_sun_update_failures_total`         | counter | Failed solar schedule recalculations.                                                                            |
| `candle_sun_no_schedule_forced_off_total`  | counter | Times output was forced off because no valid solar schedule was available.                                       |
| `candle_i2c_frames_rendered_total`         | counter | Frames written to the LED driver.                                                                                |
| `candle_i2c_frames_skipped_total`          | counter | Frames skipped because data was unchanged.                                                                       |
| `candle_i2c_errors_total`                  | counter | Animation frame pushes that failed and triggered error handling.                                                 |
| `candle_i2c_recoveries_total`              | counter | Successful I2C recoveries.                                                                                       |
| `candle_i2c_recovery_failures_total`       | counter | Failed I2C recoveries.                                                                                           |
| `candle_i2c_clock_hz`                      | gauge   | Current I2C bus clock.                                                                                           |
| `candle_i2c_clock_fallback_active`         | gauge   | `1` when fallback slow-clock mode is active.                                                                     |
| `candle_i2c_clock_fallbacks_total`         | counter | Clock fallback activations.                                                                                      |
| `candle_i2c_clock_restores_total`          | counter | Clock restorations after stable transfers.                                                                       |
| `candle_i2c_stage_*`                       | counter | I2C operation attempts, successes, failures, retries, and bytes by stage.                                        |
| `candle_i2c_end_transmission_errors_total` | counter | `Wire.endTransmission()` errors grouped by code.                                                                 |
