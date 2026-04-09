function createStatusMessage(id, text, className) {
  const element = document.createElement('div');
  element.id = id;
  element.className = className;
  element.textContent = text;
  return element;
}

function parseJsonResponse(response) {
  return response.json().then((json) => ({
    ok: response.ok,
    json
  }));
}

const settingsFormEl = document.getElementById('settingsForm');
const statusEl = document.getElementById('status');
const deviceTitleEl = document.getElementById('deviceTitle');
const btnOn = document.getElementById('candleOn');
const autoModeEl = document.getElementById('autoMode');
const brightnessEl = document.getElementById('brightness');
const brightnessValueEl = document.getElementById('brightnessValue');
const saveBtnEl = document.getElementById('saveBtn');
const resetBtnEl = document.getElementById('resetBtn');
const devnameEl = document.getElementById('devname');
const ssidEl = document.getElementById('ssid');
const passwordEl = document.getElementById('password');
const ntpServerEl = document.getElementById('ntp_server');
const ntpTimezoneEl = document.getElementById('ntp_timezone');
const latEl = document.getElementById('lat');
const lonEl = document.getElementById('lon');
const waitEl = createStatusMessage(
  'waitConnection',
  'ожидание подключения...',
  'status-message status-message--waiting'
);
const errorEl = createStatusMessage(
  'fatalError',
  'непредвиденная ошибка...',
  'status-message status-message--error'
);
const settingsInputs = [
  ...Array.from(document.querySelectorAll('.settings-details input')),
  autoModeEl
].filter(Boolean);
const kReloadDelayMs = 8000;
const kDateRefreshIntervalMs = 60000;

let dateRefreshTimer = null;
let lastManualBrightness = 50;
let liveCandleOn = false;
let liveAutoCandleOn = false;
let liveSunMode = 'unknown';

document.body.prepend(errorEl);
document.body.prepend(waitEl);

function hideSettingsView() {
  settingsFormEl.style.display = 'none';
  statusEl.style.display = 'none';
}

function removeWaitElement() {
  if (waitEl.parentNode) {
    waitEl.remove();
  }
}

function updateAutoModeTitle() {
  if (autoModeEl) {
    autoModeEl.title = `Текущий авто-режим: ${liveSunMode}`;
  }
}

function normalizeBrightnessPercent(value) {
  const parsed = parseInt(value, 10);
  if (!Number.isInteger(parsed) || parsed <= 0) {
    return 0;
  }

  if (parsed > 100) {
    return Math.round((parsed * 100) / 255);
  }

  return parsed;
}

function normalizeCoordinateText(value) {
  return String(value ?? '')
    .trim()
    .replace(/,/g, '.');
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
  const normalized = normalizeCoordinateText(value);
  return normalized || '';
}

function showFatalError(message = 'непредвиденная ошибка...') {
  errorEl.textContent = message;
  hideSettingsView();
  removeWaitElement();
  errorEl.style.display = '';
}

function syncManualToggleState() {
  const autoMode = !!(autoModeEl && autoModeEl.checked);
  const currentBrightness = normalizeBrightnessPercent(brightnessEl.value);

  if (currentBrightness > 0) {
    lastManualBrightness = currentBrightness;
  }

  btnOn.disabled = autoMode;
  btnOn.checked = autoMode ? !!liveAutoCandleOn : !!liveCandleOn;
}

function updateUI() {
  const autoMode = !!(autoModeEl && autoModeEl.checked);
  const currentBrightness = normalizeBrightnessPercent(brightnessEl.value);
  const previousBrightness = normalizeBrightnessPercent(window._lastSettings?.brightness ?? 0);

  if (!autoMode) {
    if (currentBrightness <= 0) {
      liveCandleOn = false;
    } else if (!liveCandleOn && previousBrightness <= 0) {
      liveCandleOn = true;
    }
  }

  brightnessValueEl.textContent = `${brightnessEl.value}%`;
  syncManualToggleState();
}

function getSettingsDraft() {
  return {
    devname: devnameEl.value,
    ssid: ssidEl.value,
    password: passwordEl.value,
    ntp_server: ntpServerEl.value,
    ntp_timezone: ntpTimezoneEl.value,
    lat: formatCoordinateValue(latEl.value),
    lon: formatCoordinateValue(lonEl.value),
    autoMode: !!(autoModeEl && autoModeEl.checked)
  };
}

function updateSaveButtonState() {
  if (!saveBtnEl || !window._lastSettings) {
    return;
  }

  const draft = getSettingsDraft();
  const isDirty =
    draft.devname !== (window._lastSettings.devname || '') ||
    draft.ssid !== (window._lastSettings.ssid || '') ||
    draft.password !== (window._lastSettings.password || '') ||
    draft.ntp_server !== (window._lastSettings.ntp_server || '') ||
    draft.ntp_timezone !== (window._lastSettings.ntp_timezone || '') ||
    draft.lat !== formatCoordinateValue(window._lastSettings.lat ?? '') ||
    draft.lon !== formatCoordinateValue(window._lastSettings.lon ?? '') ||
    draft.autoMode !== !!window._lastSettings.autoMode;

  saveBtnEl.disabled = !isDirty;
}

function updateDate(dateText, isValid = true) {
  statusEl.textContent = dateText || '--:-- --.--.----';
  statusEl.style.opacity = isValid ? '1' : '0.75';
}

function showRestartNotice(message = 'устройство перезагружается...') {
  waitEl.textContent = message;
  waitEl.style.display = '';
  hideSettingsView();
  document.body.prepend(waitEl);
}

function scheduleReload() {
  window.setTimeout(() => window.location.reload(), kReloadDelayMs);
}

function normalizeSettings(raw) {
  if (raw && raw.wifi) {
    const brightness = normalizeBrightnessPercent(raw.brightness);
    return {
      devname: raw.wifi.devname || '',
      name: raw.wifi.name || '',
      ssid: raw.wifi.ssid || '',
      password: raw.wifi.password || '',
      ntp_server: raw.ntp?.ntp_server || '',
      ntp_timezone: raw.ntp?.ntp_timezone || '',
      lat: raw.location?.lat ?? '',
      lon: raw.location?.lng ?? '',
      autoMode: raw.location?.enabled !== false,
      brightness,
      candleOn:
        typeof raw.candleOn === 'boolean' ? raw.candleOn : brightness > 0,
      autoCandleOn:
        typeof raw.autoCandleOn === 'boolean' ? raw.autoCandleOn : brightness > 0,
      sunMode: raw.sunMode || 'unknown'
    };
  }

  if (raw) {
    return {
      ...raw,
      brightness: normalizeBrightnessPercent(raw.brightness)
    };
  }

  return {};
}

function updateDeviceName(settings) {
  const deviceName = (settings.name || settings.devname || 'Свеча ESP32').trim();
  if (deviceTitleEl) {
    deviceTitleEl.textContent = deviceName;
  }
  document.title = `${deviceName} — управление`;
}

function fetchDate() {
  return fetch('/api/date', { cache: 'no-store' })
    .then(parseJsonResponse)
    .then(({ ok, json }) => {
      if (!ok) {
        throw new Error(json.message || 'date fetch failed');
      }

      updateDate(json.date, json.valid !== false);
      if (typeof json.candleOn === 'boolean') {
        liveCandleOn = json.candleOn;
      }
      if (typeof json.autoCandleOn === 'boolean') {
        liveAutoCandleOn = json.autoCandleOn;
      }

      liveSunMode = json.sunMode || 'unknown';
      updateAutoModeTitle();
      syncManualToggleState();
      return json;
    })
    .catch((err) => {
      console.error('Ошибка запроса /api/date:', err);
      return null;
    });
}

function startDateUpdates() {
  fetchDate();
  if (dateRefreshTimer) {
    clearInterval(dateRefreshTimer);
  }
  dateRefreshTimer = setInterval(fetchDate, kDateRefreshIntervalMs);
}

function sendBrightness() {
  const value = normalizeBrightnessPercent(brightnessEl.value);
  if (value < 0 || value > 100) {
    return Promise.resolve();
  }

  const autoMode = !!(autoModeEl && autoModeEl.checked);
  const previousBrightness = normalizeBrightnessPercent(window._lastSettings?.brightness ?? 0);
  let candleOn = autoMode ? liveAutoCandleOn : btnOn.checked;

  if (!autoMode) {
    if (value <= 0) {
      liveCandleOn = false;
      candleOn = false;
    } else if (!candleOn && previousBrightness <= 0) {
      liveCandleOn = true;
      candleOn = true;
      btnOn.checked = true;
    }
  }

  const query = new URLSearchParams({
    value: String(value),
    candleOn: candleOn ? '1' : '0',
    autoMode: autoMode ? '1' : '0'
  });

  return fetch(`/api/brightness?${query.toString()}`, {
    method: 'POST'
  })
    .then(parseJsonResponse)
    .then(({ ok, json }) => {
      if (!ok || json.status !== 'ok') {
        throw new Error(json.message || 'brightness update failed');
      }

      if (typeof json.candleOn === 'boolean') {
        liveCandleOn = json.candleOn;
      }
      if (typeof json.autoCandleOn === 'boolean') {
        liveAutoCandleOn = json.autoCandleOn;
      }
      if (typeof json.autoMode === 'boolean' && autoModeEl) {
        autoModeEl.checked = json.autoMode;
      }
      if (json.sunMode) {
        liveSunMode = json.sunMode;
      }

      if (window._lastSettings) {
        window._lastSettings.autoMode = autoMode;
        window._lastSettings.candleOn = liveCandleOn;
        window._lastSettings.brightness = value;
      }
      if (value > 0) {
        lastManualBrightness = value;
      }

      updateAutoModeTitle();
      syncManualToggleState();
      updateSaveButtonState();
      return json;
    })
    .catch((err) => {
      console.error('Ошибка запроса /api/brightness:', err);
      return null;
    });
}

btnOn.onchange = function () {
  if (autoModeEl && autoModeEl.checked) {
    syncManualToggleState();
    return;
  }

  const currentBrightness = normalizeBrightnessPercent(brightnessEl.value);
  liveCandleOn = btnOn.checked;

  if (btnOn.checked && currentBrightness <= 0) {
    brightnessEl.value = lastManualBrightness > 0 ? lastManualBrightness : 50;
  }

  updateUI();
  sendBrightness();
};

if (autoModeEl) {
  autoModeEl.onchange = function () {
    if (autoModeEl.checked) {
      fetchDate().finally(() => {
        syncManualToggleState();
        sendBrightness();
      });
      return;
    }

    liveCandleOn = liveAutoCandleOn;
    syncManualToggleState();
    sendBrightness();
  };
}

brightnessEl.oninput = updateUI;
brightnessEl.onchange = function () {
  sendBrightness();
};

settingsInputs.forEach((input) => {
  input.addEventListener('input', updateSaveButtonState);
  input.addEventListener('change', updateSaveButtonState);
});

settingsFormEl.onsubmit = function (event) {
  event.preventDefault();

  const lat = parseCoordinateValue(latEl.value);
  const lon = parseCoordinateValue(lonEl.value);

  if (latEl.value.trim() && lat === null) {
    console.error('Некорректное значение широты:', latEl.value);
    return;
  }

  if (lonEl.value.trim() && lon === null) {
    console.error('Некорректное значение долготы:', lonEl.value);
    return;
  }

  latEl.value = formatCoordinateValue(latEl.value);
  lonEl.value = formatCoordinateValue(lonEl.value);

  const data = {
    devname: devnameEl.value,
    ssid: ssidEl.value,
    password: passwordEl.value,
    ntp_server: ntpServerEl.value,
    ntp_timezone: ntpTimezoneEl.value,
    lat,
    lon,
    autoMode: !!(autoModeEl && autoModeEl.checked),
    brightness: normalizeBrightnessPercent(brightnessEl.value),
    candleOn: btnOn.checked
  };

  fetch('/api/save', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(data)
  })
    .then(parseJsonResponse)
    .then(({ ok, json }) => {
      if (!ok || json.status !== 'ok') {
        throw new Error(json.message || 'save failed');
      }

      showRestartNotice('настройки сохранены, устройство перезагружается...');
      scheduleReload();
      return json;
    })
    .catch((err) => {
      console.error('Ошибка запроса /api/save:', err);
    });
};

// --- Модальное окно подтверждения сброса ---
function showResetModal() {
  let modal = document.getElementById('resetModal');

  if (!modal) {
    modal = document.createElement('div');
    modal.id = 'resetModal';
    modal.innerHTML = `
      <div class="modal-backdrop"></div>
      <div class="modal-content">
        <div class="modal-title">Очистить Wi‑Fi?</div>
        <div class="modal-text">Будут очищены только SSID и пароль. Остальные настройки не изменятся. Продолжить?</div>
        <div class="modal-actions">
          <button id="modalCancel" class="btn btn-secondary">Отмена</button>
          <button id="modalConfirm" class="btn">Сбросить</button>
        </div>
      </div>
    `;
    document.body.appendChild(modal);
  }

  modal.style.display = 'flex';

  document.getElementById('modalCancel').onclick = function () {
    modal.style.display = 'none';
  };
  document.getElementById('modalConfirm').onclick = function () {
    resetToDefaults();
    modal.style.display = 'none';
  };
}

function resetToDefaults() {
  showRestartNotice();

  fetch('/api/reset', {
    method: 'POST'
  })
    .then(parseJsonResponse)
    .then(({ ok, json }) => {
      if (!ok || json.status !== 'ok') {
        throw new Error(json.message || 'reset failed');
      }
      return json;
    })
    .catch((err) => {
      console.error('Ошибка запроса /api/reset:', err);
    })
    .finally(() => {
      scheduleReload();
    });
}

resetBtnEl.onclick = showResetModal;

// Загрузка настроек с микроконтроллера при старте
function applySettings(settings) {
  window._lastSettings = settings;

  devnameEl.value = settings.devname || '';
  ssidEl.value = settings.ssid || '';
  passwordEl.value = settings.password || '';
  ntpServerEl.value = settings.ntp_server || '';
  ntpTimezoneEl.value = settings.ntp_timezone || '';
  latEl.value = formatCoordinateValue(settings.lat);
  lonEl.value = formatCoordinateValue(settings.lon);

  if (autoModeEl) {
    autoModeEl.checked = settings.autoMode !== false;
  }

  brightnessEl.value = settings.brightness != null ? normalizeBrightnessPercent(settings.brightness) : 0;
  liveCandleOn = settings.candleOn != null ? !!settings.candleOn : (settings.brightness ?? 0) > 0;
  liveAutoCandleOn =
    settings.autoCandleOn != null ? !!settings.autoCandleOn : liveCandleOn;

  if ((settings.brightness ?? 0) > 0) {
    lastManualBrightness = settings.brightness;
  }

  liveSunMode = settings.sunMode || 'unknown';
  updateAutoModeTitle();
  updateDeviceName(settings);
  updateUI();
  updateSaveButtonState();
  startDateUpdates();

  settingsFormEl.style.display = '';
  statusEl.style.display = '';
}

fetch('/settings.json', { cache: 'no-store' })
  .then((response) => {
    if (!response.ok) {
      throw new Error('settings.json not available');
    }
    return response.json();
  })
  .then((settings) => {
    applySettings(normalizeSettings(settings));
    removeWaitElement();
  })
  .catch(() => {
    fetch('/api/settings')
      .then(parseJsonResponse)
      .then(({ ok, json }) => {
        if (!ok) {
          throw new Error('api settings unavailable');
        }
        applySettings(normalizeSettings(json));
        removeWaitElement();
      })
      .catch(() => {
        showFatalError('не удалось получить настройки устройства');
      });
  });
