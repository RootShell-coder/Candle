# HTTP API

Веб-сервер слушает на порту **80**. Все API-ответы имеют тип `application/json`.

---

## Обзор эндпоинтов

| Метод | URL               | Описание                                 |
| ----- | ----------------- | ---------------------------------------- |
| GET   | `/`               | Веб-интерфейс (`index.html` из LittleFS) |
| GET   | `/api/settings`   | Получить текущие настройки               |
| POST  | `/api/save`       | Сохранить настройки                      |
| GET   | `/api/date`       | Текущее время и солнечный режим          |
| GET   | `/api/sun`        | Положение солнца и расписание дня        |
| GET   | `/api/moon`       | Фаза луны                                |
| GET   | `/api/brightness` | Получить яркость                         |
| POST  | `/api/brightness` | Установить яркость                       |
| POST  | `/api/reset`      | Очистка Wi-Fi учётных данных             |
| GET   | `/metrics`        | Метрики Prometheus                       |

---

## GET `/api/settings`

Возвращает все текущие настройки устройства.

**Ответ 200:**

```json
{
  "devname": "candle",
  "name": "Candle light",
  "ssid": "MyNetwork",
  "password": "secret",
  "ntp_server": "pool.ntp.org",
  "ntp_timezone": "Europe/Moscow",
  "lat": 55.29592,
  "lon": 35.58514,
  "autoMode": true,
  "sunMode": "Civil",
  "autoCandleOn": true,
  "brightness": 16,
  "candleOn": true
}
```

| Поле           | Тип    | Описание                                                                      |
| -------------- | ------ | ----------------------------------------------------------------------------- |
| `devname`      | string | Сетевое имя устройства                                                        |
| `name`         | string | Отображаемое имя                                                              |
| `ssid`         | string | Имя Wi-Fi сети                                                                |
| `password`     | string | Пароль Wi-Fi                                                                  |
| `ntp_server`   | string | Адрес NTP-сервера                                                             |
| `ntp_timezone` | string | Часовой пояс                                                                  |
| `lat`          | float  | Широта                                                                        |
| `lon`          | float  | Долгота                                                                       |
| `autoMode`     | bool   | Автоматический режим по солнцу                                                |
| `sunMode`      | string | Текущий солнечный режим (`Day`, `Civil`, `Nautical`, `Astronomical`, `Night`) |
| `autoCandleOn` | bool   | Должна ли свеча быть включена сейчас по расписанию                            |
| `brightness`   | int    | Яркость 0..100 %                                                              |
| `candleOn`     | bool   | Ручное состояние свечи                                                        |

---

## POST `/api/save`

Сохраняет настройки и перезапускает устройство (~750 мс).

**Тело запроса** — JSON с теми же полями, что возвращает `/api/settings` (все поля необязательны):

```json
{
  "devname": "candle",
  "name": "Candle light",
  "ssid": "MyNetwork",
  "password": "secret",
  "ntp_server": "pool.ntp.org",
  "ntp_timezone": "Europe/Moscow",
  "lat": 55.29592,
  "lon": 35.58514,
  "autoMode": true,
  "brightness": 50,
  "candleOn": true
}
```

Поле `brightness` принимается в процентах (0..100). Значения 101..255 трактуются как устаревший формат и конвертируются автоматически.
Поле `candleOn` учитывается только при `autoMode = false`.
Если `brightness = 0` — `candleOn` принудительно становится `false`.

**Ответ 200:**

```json
{
  "status": "ok",
  "restarting": true,
  "message": "settings saved, device restarting"
}
```

**Ответ 400** — невалидный JSON:

```json
{ "status": "error", "message": "invalid JSON payload" }
```

**Ответ 500** — ошибка записи на LittleFS:

```json
{ "status": "error", "message": "failed to save settings" }
```

---

## GET `/api/date`

Возвращает текущее местное время и состояние свечи. Полезен для обновления UI без полной перезагрузки настроек.
Ответ не кешируется (`Cache-Control: no-store`).

**Ответ 200:**

```json
{
  "valid": true,
  "date": "14:35 28.05.2026",
  "sunMode": "Day",
  "autoCandleOn": false,
  "candleOn": false
}
```

| Поле           | Тип    | Описание                                                                                        |
| -------------- | ------ | ----------------------------------------------------------------------------------------------- |
| `valid`        | bool   | `true` — время синхронизировано через NTP                                                       |
| `date`         | string | Местное время в формате `HH:MM DD.MM.YYYY`. При отсутствии синхронизации — `"--:-- --.--.----"` |
| `sunMode`      | string | Текущий солнечный режим                                                                         |
| `autoCandleOn` | bool   | Должна ли свеча быть включена по расписанию                                                     |
| `candleOn`     | bool   | Фактическое состояние свечи                                                                     |

---

## GET `/api/sun`

Вычисляет положение солнца и полное расписание событий на текущие сутки.
Ответ не кешируется (`Cache-Control: no-store`).

**Query-параметры** (необязательны — по умолчанию используются координаты из настроек):

| Параметр        | Описание                                                           |
| --------------- | ------------------------------------------------------------------ |
| `lat`           | Широта (−90..90). Принимаются числа и строки с точкой или запятой. |
| `lon` или `lng` | Долгота (−180..180).                                               |

**Ответ 200:**

```json
{
  "status": "ok",
  "validTime": true,
  "timestamp": 1748425800,
  "timezoneOffsetMinutes": 180,
  "sunMode": "Day",
  "location": {   "lat": 55.29592, "lon": 35.58514, },
  "date": { "year": 2026, "month": 5, "day": 28 },
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
    { "minute": 0, "azimuth": 2.1, "elevation": -23.4, "mode": "Night" },
    { "minute": 10, "azimuth": 4.8, "elevation": -22.1, "mode": "Night" }
  ]
}
```

Поля `events.*Begin` / `.*End` — минута суток (0..1439) от полуночи по местному времени.
Массив `path` — выборка через 10 минут (144 точки) для построения графика дня.

**Ошибки:**

| Код | Сообщение                                                  |
| --- | ---------------------------------------------------------- |
| 400 | `invalid lat` / `invalid lon` / `coordinates out of range` |
| 500 | Ошибка вычисления положения или событий                    |
| 503 | Недостаточно памяти для формирования ответа                |

---

## GET `/api/moon`

Вычисляет текущую фазу луны по синодическому периоду (алгоритм Жана Мееса).
Ответ не кешируется (`Cache-Control: no-store`).

Требует синхронизированного времени от NTP. При отсутствии времени возвращает `503`.

**Ответ 200:**

```json
{
  "status": "ok",
  "validTime": true,
  "timestamp": 1748425800,
  "ageDays": 14.73,
  "illumination": 0.98,
  "phase": 4,
  "phaseName": "Full Moon"
}
```

| Поле           | Тип    | Описание                                                                 |
| -------------- | ------ | ------------------------------------------------------------------------ |
| `status`       | string | `"ok"` при успехе                                                        |
| `validTime`    | bool   | `true` — время синхронизировано через NTP                                |
| `timestamp`    | uint32 | Unix timestamp момента вычисления (UTC)                                  |
| `ageDays`      | float  | Возраст луны в сутках с последнего новолуния, [0 .. 29.53)               |
| `illumination` | float  | Доля освещённого диска [0 .. 1], вычисляется как (1 − cos(2π·age/T)) / 2 |
| `phase`        | int    | Индекс дискретной фазы [0 .. 7] (см. таблицу ниже)                       |
| `phaseName`    | string | Название фазы на английском                                              |

### Фазы луны

| Индекс | `phaseName`     | Русское название   | Диапазон возраста (сут) |
| ------ | --------------- | ------------------ | ----------------------- |
| 0      | New Moon        | Новолуние          | 0 .. 1.85               |
| 1      | Waxing Crescent | Молодой месяц      | 1.85 .. 5.54            |
| 2      | First Quarter   | Первая четверть    | 5.54 .. 9.22            |
| 3      | Waxing Gibbous  | Прибывающая луна   | 9.22 .. 12.91           |
| 4      | Full Moon       | Полнолуние         | 12.91 .. 16.60          |
| 5      | Waning Gibbous  | Убывающая луна     | 16.60 .. 20.28          |
| 6      | Last Quarter    | Последняя четверть | 20.28 .. 23.97          |
| 7      | Waning Crescent | Стареющий месяц    | 23.97 .. 29.53          |

Границы приблизительны — деление цикла на 8 равных секторов по ~3.69 сут каждый.

**Ошибки:**

| Код | Сообщение                                      |
| --- | ---------------------------------------------- |
| 503 | `time not available` — NTP не синхронизировано |

---

## GET `/api/brightness`

Возвращает текущую яркость.

**Ответ 200:**

```json
{ "status": "ok", "brightness": 50 }
```

---

## POST `/api/brightness`

Устанавливает яркость с немедленным применением к матрице и сохранением в `settings.json`.

**Параметры** (form-data или query-string):

| Параметр                 | Обязателен | Описание                                                                           |
| ------------------------ | ---------- | ---------------------------------------------------------------------------------- |
| `value` или `brightness` | да         | Яркость 0..100 %. Значения 101..255 конвертируются.                                |
| `autoMode`               | нет        | `0`/`1`/`true`/`false` — включить/выключить автоматический режим                   |
| `candleOn`               | нет        | `0`/`1`/`true`/`false` — ручное включение свечи (учитывается при `autoMode=false`) |

**Ответ 200:**

```json
{
  "status": "ok",
  "brightness": 50,
  "candleOn": true,
  "autoMode": false,
  "autoCandleOn": false,
  "sunMode": "Night"
}
```

**Ответ 400** — некорректное значение яркости:

```json
{ "status": "error", "message": "brightness must be an integer percent from 0 to 100" }
```

---

## POST `/api/reset`

Очищает только Wi-Fi учётные данные (`ssid` и `password` сбрасываются в пустую строку), сохраняет конфигурацию и перезапускает устройство (~750 мс).
Все остальные настройки (NTP, координаты, яркость и пр.) **не изменяются**.

**Ответ 200:**

```json
{ "status": "ok", "message": "Wi-Fi credentials cleared, device restarting" }
```

---

## GET `/metrics`

Метрики в формате **Prometheus text exposition** (`text/plain; version=0.0.4`).

### Группы метрик

#### Wi-Fi

| Метрика                              | Тип     | Описание             |
| ------------------------------------ | ------- | -------------------- |
| `candle_wifi_connect_attempts_total` | counter | Попытки подключения  |
| `candle_wifi_connected_total`        | counter | Успешные подключения |
| `candle_wifi_disconnects_total`      | counter | Разрывы соединения   |

#### HTTP

| Метрика                           | Тип     | Описание            |
| --------------------------------- | ------- | ------------------- |
| `candle_http_requests_total`      | counter | Входящих запросов   |
| `candle_prometheus_scrapes_total` | counter | Запросов `/metrics` |

#### NTP

| Метрика                     | Тип     | Описание                                     |
| --------------------------- | ------- | -------------------------------------------- |
| `candle_ntp_syncs_total`    | counter | Успешных синхронизаций                       |
| `candle_ntp_failures_total` | counter | Ошибок NTP                                   |
| `candle_ntp_drift_seconds`  | gauge   | Дрейф времени при последней синхронизации, с |

#### I2C / Матрица

| Метрика                                              | Тип     | Описание                                    |
| ---------------------------------------------------- | ------- | ------------------------------------------- |
| `candle_i2c_clock_hz`                                | gauge   | Текущая частота I2C, Гц                     |
| `candle_i2c_clock_fallback_active`                   | gauge   | `1` — активен режим пониженной частоты      |
| `candle_i2c_current_page`                            | gauge   | Текущая активная страница матрицы (0 или 1) |
| `candle_i2c_consecutive_errors`                      | gauge   | Последовательных ошибок подряд              |
| `candle_i2c_max_consecutive_errors`                  | gauge   | Максимум ошибок подряд за всё время         |
| `candle_i2c_errors_total`                            | counter | Всего ошибок I2C                            |
| `candle_i2c_last_render_duration_milliseconds`       | gauge   | Длительность последнего рендера кадра, мс   |
| `candle_i2c_max_render_duration_milliseconds`        | gauge   | Максимальная длительность рендера, мс       |
| `candle_i2c_bus_bytes_written_total`                 | counter | Байт записано в шину                        |
| `candle_i2c_frames_rendered_total`                   | counter | Отрендерено кадров                          |
| `candle_i2c_frames_skipped_total`                    | counter | Пропущено кадров                            |
| `candle_i2c_last_frame_changed_bytes`                | gauge   | Изменённых байт в последнем кадре           |
| `candle_i2c_recoveries_total`                        | counter | Попыток восстановления шины                 |
| `candle_i2c_recovery_failures_total`                 | counter | Неудачных восстановлений                    |
| `candle_i2c_last_recovery_duration_milliseconds`     | gauge   | Длительность последнего восстановления, мс  |
| `candle_i2c_stage_attempts_total{stage=...}`         | counter | Попыток по стадии                           |
| `candle_i2c_stage_success_total{stage=...}`          | counter | Успехов по стадии                           |
| `candle_i2c_stage_failures_total{stage=...}`         | counter | Ошибок по стадии                            |
| `candle_i2c_stage_retries_total{stage=...}`          | counter | Повторных попыток по стадии                 |
| `candle_i2c_end_transmission_errors_total{code=...}` | counter | Ошибок `Wire.endTransmission` по коду       |
| `candle_i2c_last_error_code`                         | gauge   | Код последней ошибки Wire                   |

Стадии I2C (`stage`): `Init`, `SelectPage`, `WriteChunk`, `WriteFrame`, `FlipPage`, `Recover`.

#### Свет / Яркость

| Метрика             | Тип   | Описание                 |
| ------------------- | ----- | ------------------------ |
| `candle_brightness` | gauge | Текущая яркость 0..100 % |

#### Солнечное расписание

| Метрика                                   | Тип     | Описание                                                            |
| ----------------------------------------- | ------- | ------------------------------------------------------------------- |
| `candle_sun_mode`                         | gauge   | Текущий режим (0=Day, 1=Civil, 2=Nautical, 3=Astronomical, 4=Night) |
| `candle_sun_controlled`                   | gauge   | `1` — управление по солнцу включено                                 |
| `candle_sun_candle_enabled`               | gauge   | `1` — свеча включена по расписанию                                  |
| `candle_sun_cache_loaded`                 | gauge   | `1` — кэш расписания загружен                                       |
| `candle_sun_fetch_in_progress`            | gauge   | `1` — идёт обновление расписания                                    |
| `candle_sun_attempts_today`               | gauge   | Попыток обновить расписание сегодня                                 |
| `candle_sun_refresh_requests_total`       | counter | Запросов обновления расписания                                      |
| `candle_sun_updates_total`                | counter | Успешных обновлений расписания                                      |
| `candle_sun_update_failures_total`        | counter | Ошибок обновления расписания                                        |
| `candle_sun_no_schedule_forced_off_total` | counter | Принудительных выключений из-за отсутствия расписания               |
| `candle_sun_schedule_updated_at`          | gauge   | Unix timestamp последнего обновления расписания                     |
| `candle_sun_sunrise_minutes`              | gauge   | Восход — минута суток                                               |
| `candle_sun_sunset_minutes`               | gauge   | Закат — минута суток                                                |

---

## Статические файлы

Все файлы из корня LittleFS раздаются напрямую: `/style.css`, `/app.js`, `/anim.html`, `/sun.html` и т. д.
