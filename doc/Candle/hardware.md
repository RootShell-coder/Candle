# Hardware Pins

Firmware `1.5.3` keeps board-specific LED pins in `platformio.ini` build flags.

## Target Board

The default PlatformIO environment is `esp32s3zero`:

```ini
[env:esp32s3zero]
board = lolin_s3_mini
framework = arduino
board_build.flash_size = 4MB
board_build.filesystem = littlefs
```

The partition table is `partitions.csv`; see [web_update.md](web_update.md) for the OTA layout.

## Matrix PWM Controller

The candle matrix is driven by the IS31FL3731 over I2C.

```ini
-D I2C_SDA_PIN=8
-D I2C_SCL_PIN=9
-D I2C_ISHUTD_PIN=4
```

| Flag             | Description                                  |
| ---------------- | -------------------------------------------- |
| `I2C_SDA_PIN`    | I2C SDA pin for the IS31FL3731.              |
| `I2C_SCL_PIN`    | I2C SCL pin for the IS31FL3731.              |
| `I2C_ISHUTD_PIN` | IS31FL3731 shutdown pin. Use `-1` if unused. |

Runtime behavior:

- Default I2C clock is `50000` Hz.
- After repeated transfer errors the driver can temporarily fall back to `25000` Hz.
- With `I2C_ISHUTD_PIN >= 0`, the matrix can be put into hardware shutdown when output brightness is zero and no smooth transition is active.

## Moon WS2812 LED

The optional moon LED is a separate single WS2812 pixel. It is not connected to the IS31FL3731 matrix.

```ini
-D MOON_LED_PIN=21
```

| Flag           | Description                                           |
| -------------- | ----------------------------------------------------- |
| `MOON_LED_PIN` | GPIO used as WS2812 data output. Use `-1` to disable. |

Behavior:

- The LED can light only while the main candle output is active.
- Output brightness is `moon illumination * moonLed.maxBrightness`.
- Color hue is configured from the Moon page and stored as `moonLed.hue`.
- The firmware uses `neopixelWrite()` for the single WS2812 pixel.
- `/api/settings` and `/api/moon` expose `moonLed.hardwareEnabled`; it is `true` only when `MOON_LED_PIN >= 0`.

Electrical notes:

- WS2812 power is 5 V; use a common GND with the ESP32.
- A 3.3 V to 5 V level shifter on the data line is recommended.
- Add current limiting and power decoupling appropriate for the LED module.
