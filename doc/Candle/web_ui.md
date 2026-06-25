# Web UI

The web UI is served from LittleFS under `data/`. Static files are exposed through `server.serveStatic("/", LittleFS, "/")` with `Cache-Control: no-store, max-age=0`; dynamic JSON endpoints also avoid caching where live state is involved.

## Pages

| Page            | Purpose                                                                |
| --------------- | ---------------------------------------------------------------------- |
| `index.html`    | Main candle control, live status cards, brightness and mode controls.  |
| `settings.html` | Device hostname, Wi-Fi, NTP, manual time, and location settings.       |
| `update.html`   | Combined firmware and LittleFS web update.                             |
| `anim.html`     | Matrix animation preview and technical metrics from `/metrics`.        |
| `sun.html`      | Sun path canvas, current sun state, and twilight ranges.               |
| `moon.html`     | Moon phase canvas, lunar cycle, and optional WS2812 moon LED controls. |

Navigation links are present on all primary pages: main page, settings, update, matrix, sun, and moon.

## Runtime Data Flow

Shared scripts load `/api/settings` on startup to obtain:

- device name, firmware version, build commit, and build date;
- Wi-Fi and time validity flags;
- brightness and manual candle state;
- active control mode: manual, sun, or fixed time schedule;
- moon LED settings.

The main page posts quick control changes to `/api/brightness`. The settings page posts full settings to `/api/save`, which restarts the device after a successful save.

## Settings Page

The settings page currently exposes:

| Group    | Fields                                                  |
| -------- | ------------------------------------------------------- |
| Device   | Hostname (`devname`)                                    |
| Wi-Fi    | SSID and password                                       |
| Time     | Primary NTP, secondary NTP, timezone, manual time input |
| Location | Latitude and longitude                                  |

Manual time is sent to `/api/time` as either query/form parameter `value` or a small body. It is accepted only before NTP synchronization.

Sun mode, fixed time schedule mode, brightness, and manual candle state are controlled from the main page rather than from `settings.html`.

## CSS Layout

`data/styles/style.css` is an import-only entry point:

```css
@import url("base.css");
@import url("matrix.css");
@import url("controls.css");
@import url("sun.css");
@import url("moon.css");
@import url("app.css");
```

Keep new page-specific styles in the matching file where possible. Shared shell, panels, forms, update controls, and settings groups are in `app.css`.

## JavaScript Structure

| File                                     | Scope                                                                                 |
| ---------------------------------------- | ------------------------------------------------------------------------------------- |
| `scripts/core.js`                        | Shared helpers, element lookup, settings normalization, locale lookup, device badges. |
| `scripts/live.js`                        | Main page status polling and live brightness/mode controls.                           |
| `scripts/settings.js`                    | Settings form, manual time, Wi-Fi reset.                                              |
| `scripts/update.js`                      | Firmware/LittleFS upload flow and restart request.                                    |
| `scripts/art.js` / `scripts/art-data.js` | Matrix animation preview and metrics parsing.                                         |
| `scripts/sun.js`                         | Sun path canvas and solar event visualization.                                        |
| `scripts/moon.js`                        | Moon phase canvas and moon LED controls.                                              |
| `scripts/nav.js`                         | Navigation state helpers.                                                             |
| `scripts/app.js`                         | Page bootstrap glue.                                                                  |

## Locales

Russian locale files are split by page under `data/locales/ru/`:

| File          | Scope                         |
| ------------- | ----------------------------- |
| `common.js`   | Shared labels and app errors. |
| `index.js`    | Main page status and live UI. |
| `settings.js` | Settings page messages.       |
| `update.js`   | Update page messages.         |
| `sun.js`      | Sun page labels and errors.   |
| `moon.js`     | Moon page labels and errors.  |

Sun and moon scripts include English fallback text so missing locale keys still render usable labels.
