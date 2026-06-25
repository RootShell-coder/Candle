# Web Update

The device can update both firmware and LittleFS from the web UI. The update mechanism requires the OTA partition layout from `partitions.csv`.

Update endpoints are intended for a trusted local network and do not require authentication.

## Partition Layout

| Partition  | Type | Subtype  | Offset     | Size       |
| ---------- | ---- | -------- | ---------- | ---------- |
| `nvs`      | data | `nvs`    | `0x9000`   | `0x5000`   |
| `otadata`  | data | `ota`    | `0xe000`   | `0x2000`   |
| `app0`     | app  | `ota_0`  | `0x10000`  | `0x140000` |
| `app1`     | app  | `ota_1`  | `0x150000` | `0x140000` |
| `littlefs` | data | `spiffs` | `0x290000` | `0x170000` |

The LittleFS partition uses subtype `spiffs` for Arduino/ESP32 update compatibility, while firmware mounts it as LittleFS with label `littlefs`.

## Build Artifacts

Build firmware:

```powershell
platformio run -e esp32s3zero
```

Firmware image:

```text
.pio\build\esp32s3zero\firmware.bin
```

Build LittleFS:

```powershell
platformio run -e esp32s3zero -t buildfs
```

LittleFS image:

```text
.pio\build\esp32s3zero\littlefs.bin
```

## Web UI Flow

The update page accepts two files:

1. `firmware.bin`
2. `littlefs.bin`

The browser uploads firmware first, uploads LittleFS second, sends a restart request, waits for the device to boot again, and reloads the UI with a cache-busting query string.

## HTTP Endpoints

### Upload Firmware

```http
POST /api/update/firmware
Content-Type: application/octet-stream
X-Update-Filename: firmware.bin
```

Successful response:

```json
{
  "status": "ok",
  "restarting": false,
  "message": "update uploaded, restart deferred"
}
```

Firmware uploads target the inactive OTA application partition returned by `esp_ota_get_next_update_partition()`.

### Upload LittleFS

```http
POST /api/update/filesystem
Content-Type: application/octet-stream
X-Update-Filename: littlefs.bin
```

Successful response:

```json
{
  "status": "ok",
  "restarting": false,
  "message": "update uploaded, restart deferred"
}
```

Filesystem uploads target the `littlefs` data partition. The firmware first searches for subtype `spiffs` with label `littlefs`, then falls back to any data subtype with the same label.

### Restart After Update

```http
POST /api/update/restart
```

Successful response:

```json
{
  "status": "ok",
  "restarting": true,
  "restartScheduled": true,
  "restartInMs": 1500,
  "message": "device restarting"
}
```

### Update Status

```http
GET /api/update
```

Response:

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

## Validation

- Firmware uploads target the inactive OTA app partition.
- LittleFS uploads target the `littlefs` data partition.
- Upload size must be greater than zero.
- Upload size must fit into the target partition.
- The file extension must be `.bin` when `X-Update-Filename` is provided.
- Only one update can be active at a time.
- A successful upload does not immediately restart the device; `/api/update/restart` is a separate step.

Possible upload errors include:

| Message | Meaning |
| ------- | ------- |
| `no update file uploaded` | The final request handler ran without upload state. |
| `update file must be a .bin binary` | `X-Update-Filename` was provided without `.bin` suffix. |
| `update file is empty` | Upload size was zero. |
| `target update partition not found` | No matching OTA or LittleFS partition was found. |
| `update upload is larger than ... bytes` | Request body exceeds the target partition. |
| `another update is already in progress` | A second update was started while one was active. |
| `failed to begin update` | `Update.begin()` failed. |
| `failed to write update chunk` | A chunk write failed. |
| `failed to finalize update` | `Update.end(true)` failed. |

## Metrics

| Metric                      | Type    | Description                                        |
| --------------------------- | ------- | -------------------------------------------------- |
| `candle_ota_active`         | gauge   | `1` while an OTA upload/write operation is active. |
| `candle_ota_success_total`  | counter | Successful OTA uploads.                            |
| `candle_ota_failures_total` | counter | Failed OTA uploads.                                |
