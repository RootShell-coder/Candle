(function () {
  const locale = window.CandleLocale || {};
  const DEFAULT_TIME_ON_MINUTE = 18 * 60;
  const DEFAULT_TIME_OFF_MINUTE = 23 * 60;
  let toastTimer = null;

  const state = {
    settings: null,
    datePayload: null,
    lastManualBrightness: 50,
    liveCandleOn: false,
    liveAutoCandleOn: false,
    liveAutoTimeCandleOn: false,
    liveSunMode: 'unknown',
    workMode: 'manual',
    timeSynced: false,
    dateRefreshTimer: null
  };

  function $(id) {
    return document.getElementById(id);
  }

  function collectElements() {
    return {
      toast: $('toast'),
      status: $('status'),
      deviceTitle: $('deviceTitle'),
      deviceSubtitle: $('deviceSubtitle'),
      firmwareBadge: $('firmwareBadge'),
      updateVersion: $('updateVersion'),
      manualMode: $('manualMode'),
      autoMode: $('autoMode'),
      autoTimeMode: $('autoTimeMode'),
      manualModeStateText: $('manualModeStateText'),
      autoStateText: $('autoStateText'),
      autoTimeStateText: $('autoTimeStateText'),
      timeOn: $('timeOn'),
      timeOff: $('timeOff'),
      brightness: $('brightness'),
      brightnessValue: $('brightnessValue'),
      wifiStaStatus: $('wifiStaStatus'),
      wifiStaDetails: $('wifiStaDetails'),
      wifiApStatus: $('wifiApStatus'),
      wifiApDetails: $('wifiApDetails'),
      ntpStatus: $('ntpStatus'),
      ntpDetails: $('ntpDetails'),
      autoModeStatus: $('autoModeStatus'),
      autoModeDetails: $('autoModeDetails'),
      settingsForm: $('settingsForm'),
      saveBtn: $('saveBtn'),
      resetBtn: $('resetBtn'),
      devname: $('devname'),
      ssid: $('ssid'),
      password: $('password'),
      ntpServer: $('ntp_server'),
      ntpServer2: $('ntp_server2'),
      ntpTimezone: $('ntp_timezone'),
      manualTime: $('manualTime'),
      manualTimeBtn: $('manualTimeBtn'),
      lat: $('lat'),
      lon: $('lon'),
      firmwareFile: $('firmwareFile'),
      filesystemFile: $('filesystemFile'),
      firmwareFileMeta: $('firmwareFileMeta'),
      filesystemFileMeta: $('filesystemFileMeta'),
      updateBtn: $('updateBtn'),
      updateProgress: $('updateProgress'),
      updateStatus: $('updateStatus')
    };
  }

  function t(path, params = {}) {
    const value = path.split('.').reduce((node, key) => node && node[key], locale);
    const template = typeof value === 'string' ? value : path;
    return Object.keys(params).reduce(
      (out, key) => out.replaceAll(`{${key}}`, String(params[key])),
      template
    );
  }

  function setText(element, value) {
    if (element) {
      element.textContent = value || '--';
    }
  }

  function showToast(message, type = 'info', timeoutMs = 5000) {
    const toast = window.Candle.els.toast;
    if (!toast) {
      return;
    }

    window.clearTimeout(toastTimer);
    toast.textContent = message;
    toast.className = `toast toast--visible toast--${type}`;
    if (timeoutMs > 0) {
      toastTimer = window.setTimeout(() => {
        toast.className = 'toast';
      }, timeoutMs);
    }
  }

  function parseJsonResponse(response) {
    return response.text().then((raw) => {
      if (!raw || !raw.trim()) {
        throw new Error(`${response.url || 'API'} ${t('app.emptyResponse')}`);
      }

      try {
        return { ok: response.ok, status: response.status, json: JSON.parse(raw) };
      } catch (err) {
        const preview = raw.slice(0, 180).replace(/[\u0000-\u001f]/g, ' ');
        throw new Error(`${t('app.invalidJson')} ${response.url || 'API'}: ${preview}`);
      }
    });
  }

  function normalizeBrightnessPercent(value) {
    const parsed = parseInt(value, 10);
    if (!Number.isInteger(parsed) || parsed <= 0) {
      return 0;
    }
    return parsed > 100 ? Math.round((parsed * 100) / 255) : parsed;
  }

  function normalizeTimeMinute(value, fallback) {
    const parsed = Number(value);
    return Number.isFinite(parsed) ? Math.max(0, Math.min(1439, Math.round(parsed))) : fallback;
  }

  function normalizeHue(value) {
    const parsed = parseInt(value ?? 42, 10) || 0;
    return Math.max(0, Math.min(360, parsed));
  }

  function normalizeCoordinateText(value) {
    return String(value ?? '').trim().replace(/,/g, '.');
  }

  function parseCoordinateValue(value) {
    const normalized = normalizeCoordinateText(value);
    if (!normalized) {
      return null;
    }
    const parsed = Number(normalized);
    return Number.isFinite(parsed) ? parsed : null;
  }

  function formatCoordinateValue(value) {
    return normalizeCoordinateText(value) || '';
  }

  function formatBytes(bytes) {
    if (!Number.isFinite(bytes) || bytes <= 0) {
      return `0 ${t('units.bytes')}`;
    }

    const units = [t('units.bytes'), t('units.kilobytes'), t('units.megabytes')];
    let value = bytes;
    let unitIndex = 0;
    while (value >= 1024 && unitIndex < units.length - 1) {
      value /= 1024;
      unitIndex += 1;
    }
    return `${value.toFixed(unitIndex === 0 ? 0 : 1)} ${units[unitIndex]}`;
  }

  function normalizeMoonLed(rawMoonLed = {}) {
    return {
      enabled: rawMoonLed?.enabled === true,
      maxBrightness: normalizeBrightnessPercent(rawMoonLed?.maxBrightness ?? 25),
      hue: normalizeHue(rawMoonLed?.hue),
      currentBrightness: normalizeBrightnessPercent(rawMoonLed?.currentBrightness ?? 0),
      hardwareEnabled: rawMoonLed?.hardwareEnabled === true
    };
  }

  function normalizeTimeSchedule(rawSchedule = {}) {
    return {
      autoTimeMode: rawSchedule?.enabled === true,
      timeOnMinute: normalizeTimeMinute(rawSchedule?.onMinute, DEFAULT_TIME_ON_MINUTE),
      timeOffMinute: normalizeTimeMinute(rawSchedule?.offMinute, DEFAULT_TIME_OFF_MINUTE)
    };
  }

  function normalizeFlatTimeSchedule(raw) {
    return {
      autoTimeMode: raw.autoTimeMode === true,
      timeOnMinute: normalizeTimeMinute(raw.timeOnMinute, DEFAULT_TIME_ON_MINUTE),
      timeOffMinute: normalizeTimeMinute(raw.timeOffMinute, DEFAULT_TIME_OFF_MINUTE)
    };
  }

  function normalizeSettings(raw) {
    if (!raw) {
      return {};
    }

    if (raw.wifi) {
      const brightness = normalizeBrightnessPercent(raw.brightness);
      const timeSchedule = normalizeTimeSchedule(raw.timeSchedule);
      return {
        devname: raw.wifi.devname || '',
        name: raw.wifi.name || '',
        ssid: raw.wifi.ssid || '',
        password: raw.wifi.password || '',
        ntp_server: raw.ntp?.ntp_server || '',
        ntp_server2: raw.ntp?.ntp_server2 || '',
        ntp_timezone: raw.ntp?.ntp_timezone || '',
        lat: raw.location?.lat ?? '',
        lon: raw.location?.lng ?? '',
        autoMode: raw.location?.enabled !== false,
        ...timeSchedule,
        validTime: raw.validTime === true,
        ntpSynchronized: raw.ntpSynchronized === true,
        manualTimeAllowed: raw.manualTimeAllowed !== false,
        wifiStaConnected: raw.wifiStaConnected === true,
        wifiHasIp: raw.wifiHasIp === true,
        brightness,
        candleOn: typeof raw.candleOn === 'boolean' ? raw.candleOn : brightness > 0,
        autoCandleOn: typeof raw.autoCandleOn === 'boolean' ? raw.autoCandleOn : brightness > 0,
        autoTimeCandleOn: typeof raw.autoTimeCandleOn === 'boolean' ? raw.autoTimeCandleOn : false,
        moonLed: normalizeMoonLed(raw.moonLed),
        sunMode: raw.sunMode || 'unknown',
        firmwareVersion: raw.firmwareVersion || '',
        buildCommit: raw.buildCommit || '',
        buildDate: raw.buildDate || ''
      };
    }

    return {
      ...raw,
      ...normalizeFlatTimeSchedule(raw),
      brightness: normalizeBrightnessPercent(raw.brightness),
      validTime: raw.validTime === true,
      ntpSynchronized: raw.ntpSynchronized === true,
      manualTimeAllowed: raw.manualTimeAllowed !== false,
      wifiStaConnected: raw.wifiStaConnected === true,
      wifiHasIp: raw.wifiHasIp === true,
      moonLed: normalizeMoonLed(raw.moonLed)
    };
  }

  function updateDeviceName(settings) {
    const els = window.Candle.els;
    const deviceName = (settings.name || settings.devname || 'Candle Light').trim();
    setText(els.deviceTitle, deviceName);
    setText(els.deviceSubtitle, settings.devname ? `имя хоста: ${settings.devname}` : t('app.hostnameMissing'));
    document.title = `${deviceName} - ${t('app.deviceTitleSuffix')}`;
  }

  function updateFirmwareBadges(settings) {
    const els = window.Candle.els;
    const version = settings.firmwareVersion ? `v${settings.firmwareVersion}` : 'v--';
    const commit = settings.buildCommit && settings.buildCommit !== 'unknown' ? settings.buildCommit : '';
    const label = commit ? `${version} / ${commit}` : version;
    setText(els.firmwareBadge, version);
    setText(els.updateVersion, label);
    if (els.updateVersion) {
      els.updateVersion.title = settings.buildDate ? `Дата сборки: ${settings.buildDate}` : '';
    }
  }

  function waitForDeviceAndReload(startDelayMs = 1500) {
    const startedAt = Date.now();
    let sawOffline = false;

    window.setTimeout(function poll() {
      fetch(`/api/settings?reload=${Date.now()}`, { cache: 'no-store' })
        .then((response) => {
          if (!response.ok) {
            throw new Error('устройство еще не готово');
          }
          if (!sawOffline && Date.now() - startedAt < 12000) {
            window.setTimeout(poll, 1500);
            return;
          }
          const url = new URL(window.location.href);
          url.searchParams.set('reload', Date.now().toString());
          window.location.replace(url.toString());
        })
        .catch(() => {
          sawOffline = true;
          window.setTimeout(poll, 1500);
        });
    }, startDelayMs);
  }

  function showRestartNotice(message) {
    showToast(message, 'info', 0);
    document.body.classList.add('is-restarting');
  }

  function setTimedNavigationEnabled(enabled) {
    document.querySelectorAll('a[href$="sun.html"], a[href$="moon.html"]').forEach((link) => {
      link.classList.toggle('sun-link--disabled', !enabled);
      link.setAttribute('aria-disabled', enabled ? 'false' : 'true');
      if (enabled) {
        link.removeAttribute('tabindex');
      } else {
        link.setAttribute('tabindex', '-1');
      }
    });
  }

  document.addEventListener('click', (event) => {
    const link = event.target.closest?.('a[href$="sun.html"], a[href$="moon.html"]');
    if (link && link.getAttribute('aria-disabled') === 'true') {
      event.preventDefault();
    }
  });
  setTimedNavigationEnabled(false);

  function loadSettings() {
    return fetch('/api/settings', { cache: 'no-store' })
      .then(parseJsonResponse)
      .then(({ ok, json }) => {
        if (!ok) {
          throw new Error(json.message || t('app.settingsUnavailable'));
        }

        const settings = normalizeSettings(json);
        state.settings = settings;
        window._lastSettings = settings;
        updateDeviceName(settings);
        updateFirmwareBadges(settings);
        setTimedNavigationEnabled(settings.validTime === true);
        return settings;
      });
  }

  function refreshTimedNavigation() {
    return fetch('/api/settings', { cache: 'no-store' })
      .then(parseJsonResponse)
      .then(({ ok, json }) => {
        setTimedNavigationEnabled(ok && json.validTime === true);
        return json;
      })
      .catch(() => {
        setTimedNavigationEnabled(false);
        return null;
      });
  }

  window.Candle = {
    els: collectElements(),
    state,
    t,
    setText,
    showToast,
    parseJsonResponse,
    normalizeBrightnessPercent,
    parseCoordinateValue,
    formatCoordinateValue,
    formatBytes,
    loadSettings,
    refreshTimedNavigation,
    waitForDeviceAndReload,
    showRestartNotice,
    setTimedNavigationEnabled
  };
}());
