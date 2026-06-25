# I2C Module

`src/i2c.cpp` drives the candle LED matrix through an IS31FL3731
charlieplexed PWM controller on the I2C bus. It is the firmware module that
turns the compressed 9x16 candle animation data into PWM frames and pushes
those frames to the physical matrix.

## Responsibilities

- Initializes the ESP32 I2C bus, the IS31FL3731 device, and the optional
  hardware shutdown pin.
- Converts user brightness values from `0..100` percent into visible PWM
  values, including gamma-style scaling and a low-end visible brightness floor.
- Advances the candle animation stream from `anim9x16.h`, expands changed
  rectangles into a 144-byte frame buffer, and applies the current brightness.
- Uses the IS31FL3731 page model for double buffering: it writes the next frame
  to the inactive page and then flips the display to that page.
- Skips I2C writes when the scaled frame did not change.
- Shuts the matrix down when brightness reaches zero and wakes it again with a
  recovery pass before restoring brightness.
- Handles I2C reliability concerns: short retries, soft-fault tracking, bus
  recovery, clock fallback from 50 kHz to 25 kHz after repeated errors, and
  return to the normal clock after stable operation.
- Reports I2C, brightness, render, recovery, and frame-change state to the
  metrics module for the `/metrics` endpoint.

The public interface is intentionally small:

- `i2c_init()` prepares the bus and LED driver.
- `i2c_set_brightness()` sets the target matrix brightness.
- `i2c_anim_task()` runs one animation/update cycle and is called by the RTOS
  task loop.

## Separate Licensing

`src/i2c.cpp` is intended to be licensed separately from the rest of this
repository. It contains the project-specific LED matrix control logic,
animation frame pipeline, brightness behavior, recovery strategy, and hardware
handling for this candle device.

This file should not be treated as open source by default and should not be
redistributed as part of a public source release unless its separate license
explicitly allows that. Keep it out of public distributions or replace it with
a separately licensed implementation/stub when publishing the rest of the
project.

The separate license is useful because `src/i2c.cpp` contains product-specific
firmware behavior rather than generic integration glue. Keeping it under a
different license allows the rest of the project to be shared while preserving
control over this hardware driver and animation pipeline.

## Third-Party Dependency License

`src/i2c.cpp` also includes and uses `Adafruit_IS31FL3731.h`. In this project
that library is declared in `platformio.ini` as:

```ini
adafruit/Adafruit IS31FL3731 Library@2.0.2
```

That library is a third-party dependency from Adafruit and is licensed under
the MIT License. It is not owned by this firmware repository, even though it is
linked into the firmware during the PlatformIO build.

This is a separate issue from the private/separately licensed status of
`src/i2c.cpp`: Adafruit's library keeps its own copyright and license terms,
and this project file does not relicense Adafruit's code. Redistribution of
firmware or source packages should keep the Adafruit license notice with the
dependency, as required by the MIT License.
