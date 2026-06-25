# Offline Solar Calculations

The firmware calculates sun position and local-day events on the device. It does not call an external sunrise/sunset API.

The implementation follows the NOAA solar calculation model and is used by:

- automatic sun-based candle mode;
- `/api/sun`;
- sun position visualization;
- Prometheus sun metrics.

Firmware `1.5.3` also supports a fixed time schedule mode. That mode uses the same valid local time source but does not use solar calculations.

## Inputs

| Input   | Description                                           |
| ------- | ----------------------------------------------------- |
| `t_utc` | Unix timestamp in UTC seconds.                        |
| `lat`   | Latitude in degrees, `-90..90`.                       |
| `lon`   | Longitude in degrees, `-180..180`, east positive.     |
| `tz`    | Local UTC offset in minutes for the requested moment. |

Coordinates come from `settings.json` unless `/api/sun` query parameters override them.

## Julian Time

Julian day:

```text
JD = t_utc / 86400 + 2440587.5
```

Julian centuries from J2000:

```text
T = (JD - 2451545.0) / 36525
```

## Solar Parameters

Mean longitude of the Sun:

```text
L0 = 280.46646 + T * (36000.76983 + 0.0003032 * T)
```

Mean anomaly:

```text
M = 357.52911 + T * (35999.05029 - 0.0001537 * T)
```

Earth orbit eccentricity:

```text
e = 0.016708634 - T * (0.000042037 + 0.0000001267 * T)
```

Equation of center:

```text
C = sin(M) * (1.914602 - T * (0.004817 + 0.000014 * T))
  + sin(2M) * (0.019993 - 0.000101 * T)
  + 0.000289 * sin(3M)
```

True and apparent longitude:

```text
lambda_true = L0 + C
Omega = 125.04 - 1934.136 * T
lambda = lambda_true - 0.00569 - 0.00478 * sin(Omega)
```

Mean and corrected obliquity:

```text
epsilon0 = 23 + (26 + (21.448 - T * (46.815 + T * (0.00059 - 0.001813 * T))) / 60) / 60
epsilon = epsilon0 + 0.00256 * cos(Omega)
```

Declination:

```text
delta = asin(sin(epsilon) * sin(lambda))
```

Equation of time in minutes:

```text
y = tan(epsilon / 2)^2

EqTime = 4 * deg(
  y * sin(2L0)
  - 2e * sin(M)
  + 4e * y * sin(M) * cos(2L0)
  - 0.5 * y^2 * sin(4L0)
  - 1.25 * e^2 * sin(2M)
)
```

## Current Sun Position

True solar time in minutes:

```text
TST = utcMinutes + EqTime + 4 * lon
```

Hour angle:

```text
H = TST / 4 - 180
```

Zenith angle:

```text
cos(Z) = sin(lat) * sin(delta) + cos(lat) * cos(delta) * cos(H)
elevation = 90 - Z
```

Azimuth in range `0..360`:

```text
azimuth = norm360(atan2(sin(H), cos(H) * sin(lat) - tan(delta) * cos(lat)) + 180)
```

## Sunrise, Sunset, and Twilight

For a requested zenith threshold `Z0`:

```text
cos(H0) = cos(Z0) / (cos(lat) * cos(delta)) - tan(lat) * tan(delta)
```

Cases:

- `cos(H0) > 1`: event does not occur because the Sun is always below the threshold.
- `cos(H0) < -1`: event does not occur because the Sun is always above the threshold.
- otherwise `H0 = acos(cos(H0))`.

Local event minutes:

```text
t_morning = 720 - 4 * (lon + H0) - EqTime + tz
t_evening = 720 - 4 * (lon - H0) - EqTime + tz
```

Normalized minute of day:

```text
minute = ((round(t) mod 1440) + 1440) mod 1440
```

Thresholds:

| Event                 | Zenith       |
| --------------------- | ------------ |
| Sunrise / sunset      | `90.833` deg |
| Civil twilight        | `96` deg     |
| Nautical twilight     | `102` deg    |
| Astronomical twilight | `108` deg    |

## Sun Modes

Current sun mode for `/api/date`, `/api/sun`, metrics, and the web UI is classified from the current calculated elevation when valid time is available. This avoids event-schedule edge cases when the Sun does not cross a lower twilight boundary during the local day.

| Mode           | Elevation or state                                          |
| -------------- | ----------------------------------------------------------- |
| `day`          | `elevation >= 0`                                            |
| `civil`        | `-6 <= elevation < 0`                                       |
| `nautical`     | `-12 <= elevation < -6`                                     |
| `astronomical` | `-18 <= elevation < -12`                                    |
| `night`        | `elevation < -18`                                           |
| `time`         | Fixed time schedule mode is enabled and valid time exists.  |
| `manual`       | Sun mode and fixed time schedule mode are both disabled.    |
| `waiting_time` | Automatic mode is enabled but valid time is not available.  |
| `unknown`      | A schedule or position calculation could not be completed.  |

Brightness in sun mode:

| Solar mode | Target brightness |
| ---------- | ----------------- |
| `day` | `0` |
| `civil` | configured brightness reduced by 10 percent |
| `nautical` | configured brightness reduced by 20 percent |
| `astronomical` | configured brightness reduced by 30 percent |
| `night` | configured brightness |

## Fixed Time Schedule

The fixed schedule is stored in `settings.json` under `timeSchedule` and is mutually exclusive with sun mode.

| Field | Description |
| ----- | ----------- |
| `timeSchedule.enabled` | Enables schedule mode. |
| `timeSchedule.onMinute` | Start minute from local midnight, `0..1439`. |
| `timeSchedule.offMinute` | End minute from local midnight, `0..1439`. |

Schedule rules:

- If `onMinute == offMinute`, the schedule is always on.
- If `onMinute < offMinute`, the candle is on in `[onMinute, offMinute)`.
- If `onMinute > offMinute`, the interval crosses midnight.
- Schedule mode requires valid time; before that, firmware falls back to the manual candle state and reports `waiting_time`.

## Daily Path for Canvas

The `/api/sun` response includes `path`, a list of points used by `sun.html`. The path is calculated once per local day and cached for up to 30 hours for the same date, coordinates, and timezone offset.

- base sampling interval: 10 minutes;
- nominal base point count: 144 points per day;
- each point includes `minute`, `azimuth`, `elevation`, and elevation-derived `mode`;
- exact threshold crossing points are inserted for `0`, `-6`, `-12`, and `-18` degrees when crossings exist between adjacent samples;
- `pathMinElevation` and `pathMaxElevation` summarize the visible daily range.

The frontend draws the curve directly from these points and derives twilight ranges from the same crossing points, so the text values match the canvas boundaries.

Display bands:

| Band                  | Elevation range |
| --------------------- | --------------- |
| Day                   | `0..90`         |
| Sunset twilight band  | `-6..0`         |
| Nautical twilight     | `-12..-6`       |
| Astronomical twilight | `-18..-12`      |
| Night                 | `< -18`         |

## Polar Cases

For the official sunrise/sunset threshold:

- `isPolarDay = true` when the Sun never drops below the threshold during the local day.
- `isPolarNight = true` when the Sun never rises above the threshold during the local day.

In these cases the firmware chooses the day or night mode directly and does not rely on missing sunrise/sunset event times.
