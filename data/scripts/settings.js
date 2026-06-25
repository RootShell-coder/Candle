(function () {
  const Candle = window.Candle;
  const { els, state, t } = Candle;

  function settingsInputs() {
    return [
      els.devname,
      els.ssid,
      els.password,
      els.ntpServer,
      els.ntpServer2,
      els.ntpTimezone,
      els.lat,
      els.lon
    ].filter(Boolean);
  }

  function getSettingsDraft() {
    return {
      devname: els.devname?.value || '',
      ssid: els.ssid?.value || '',
      password: els.password?.value || '',
      ntp_server: els.ntpServer?.value || '',
      ntp_server2: els.ntpServer2?.value || '',
      ntp_timezone: els.ntpTimezone?.value || '',
      lat: Candle.formatCoordinateValue(els.lat?.value ?? ''),
      lon: Candle.formatCoordinateValue(els.lon?.value ?? '')
    };
  }

  function updateSaveButtonState() {
    if (!els.saveBtn || !state.settings) {
      return;
    }

    const draft = getSettingsDraft();
    const isDirty =
      draft.devname !== (state.settings.devname || '') ||
      draft.ssid !== (state.settings.ssid || '') ||
      draft.password !== (state.settings.password || '') ||
      draft.ntp_server !== (state.settings.ntp_server || '') ||
      draft.ntp_server2 !== (state.settings.ntp_server2 || '') ||
      draft.ntp_timezone !== (state.settings.ntp_timezone || '') ||
      draft.lat !== Candle.formatCoordinateValue(state.settings.lat ?? '') ||
      draft.lon !== Candle.formatCoordinateValue(state.settings.lon ?? '');

    els.saveBtn.disabled = !isDirty;
  }

  function validateSettingsForm() {
    const lat = Candle.parseCoordinateValue(els.lat?.value ?? '');
    const lon = Candle.parseCoordinateValue(els.lon?.value ?? '');

    if (els.lat?.value.trim() && (lat === null || lat < -90 || lat > 90)) {
      Candle.showToast(t('settings.invalidLat'), 'error');
      els.lat.focus();
      return null;
    }

    if (els.lon?.value.trim() && (lon === null || lon < -180 || lon > 180)) {
      Candle.showToast(t('settings.invalidLon'), 'error');
      els.lon.focus();
      return null;
    }

    return { lat, lon };
  }

  function formatManualTimeInput(date = new Date()) {
    const pad2 = (value) => String(value).padStart(2, '0');
    return `${pad2(date.getHours())}:${pad2(date.getMinutes())} ${pad2(date.getDate())}.${pad2(date.getMonth() + 1)}.${date.getFullYear()}`;
  }

  function normalizeManualTimeInput(value) {
    const digits = String(value || '').replace(/\D/g, '').slice(0, 12);
    let out = digits.slice(0, 2);
    if (digits.length > 2) out += `:${digits.slice(2, 4)}`;
    if (digits.length > 4) out += ` ${digits.slice(4, 6)}`;
    if (digits.length > 6) out += `.${digits.slice(6, 8)}`;
    if (digits.length > 8) out += `.${digits.slice(8, 12)}`;
    return out;
  }

  function isManualTimeComplete(value) {
    return /^\d{2}:\d{2} \d{2}\.\d{2}\.\d{4}$/.test(String(value || '').trim());
  }

  function parseManualTimeInput(value) {
    const match = /^(\d{2}):(\d{2}) (\d{2})\.(\d{2})\.(\d{4})$/.exec(String(value || '').trim());
    if (!match) {
      return null;
    }

    const hour = Number(match[1]);
    const minute = Number(match[2]);
    const day = Number(match[3]);
    const month = Number(match[4]);
    const year = Number(match[5]);
    if (
      !Number.isInteger(hour) || hour < 0 || hour > 23 ||
      !Number.isInteger(minute) || minute < 0 || minute > 59 ||
      !Number.isInteger(year) || year < 2024 || year > 2100 ||
      !Number.isInteger(month) || month < 1 || month > 12 ||
      !Number.isInteger(day) || day < 1
    ) {
      return null;
    }

    const maxDay = new Date(year, month, 0).getDate();
    if (day > maxDay) {
      return null;
    }

    return { hour, minute, day, month, year };
  }

  function refreshManualTimeFromDevice() {
    if (!els.manualTime) {
      return;
    }

    fetch('/api/date', { cache: 'no-store' })
      .then(Candle.parseJsonResponse)
      .then(({ ok, json }) => {
        if (ok && typeof json.date === 'string' && isManualTimeComplete(json.date)) {
          els.manualTime.value = json.date;
        }
      })
      .catch(() => {});
  }

  function setManualTimeState(settings = state.settings || {}) {
    const allowed = settings.ntpSynchronized !== true && settings.manualTimeAllowed !== false;
    if (els.manualTime) {
      els.manualTime.disabled = !allowed;
      if (allowed && !els.manualTime.value.trim()) {
        els.manualTime.value = formatManualTimeInput();
      }
      if (!allowed && settings.ntpSynchronized === true) {
        refreshManualTimeFromDevice();
      }
    }
    if (els.manualTimeBtn) {
      els.manualTimeBtn.disabled = !allowed;
    }
  }

  function saveManualTime() {
    if (!els.manualTime || !els.manualTimeBtn) {
      return;
    }

    const value = els.manualTime.value.trim();
    if (!parseManualTimeInput(value)) {
      Candle.showToast(t('settings.manualTimeRequired'), 'error');
      els.manualTime.focus();
      return;
    }

    els.manualTimeBtn.disabled = true;
    fetch('/api/time', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ value })
    })
      .then(Candle.parseJsonResponse)
      .then(({ ok, json }) => {
        if (!ok || json.status !== 'ok') {
          throw new Error(json.message || t('settings.manualTimeFailed'));
        }
        state.settings = {
          ...(state.settings || {}),
          validTime: json.validTime === true || json.valid === true,
          ntpSynchronized: json.ntpSynchronized === true,
          manualTimeAllowed: json.manualTimeAllowed !== false
        };
        Candle.setTimedNavigationEnabled(state.settings.validTime === true);
        setManualTimeState(state.settings);
        Candle.showToast(t('settings.manualTimeSaved'), 'info');
      })
      .catch((err) => {
        Candle.showToast(err.message || t('settings.manualTimeError'), 'error');
      })
      .finally(() => {
        setManualTimeState(state.settings);
      });
  }

  function saveSettings(event) {
    event.preventDefault();
    const coordinates = validateSettingsForm();
    if (!coordinates) {
      return;
    }

    const data = {
      devname: els.devname.value,
      ssid: els.ssid.value,
      password: els.password.value,
      ntp_server: els.ntpServer.value,
      ntp_server2: els.ntpServer2.value,
      ntp_timezone: els.ntpTimezone.value,
      lat: coordinates.lat,
      lon: coordinates.lon,
      autoMode: state.settings?.autoMode !== false,
      autoTimeMode: state.settings?.autoTimeMode === true,
      timeOnMinute: state.settings?.timeOnMinute ?? 18 * 60,
      timeOffMinute: state.settings?.timeOffMinute ?? 23 * 60,
      brightness: Candle.normalizeBrightnessPercent(state.settings?.brightness ?? 0),
      candleOn: !!state.settings?.candleOn
    };

    els.saveBtn.disabled = true;
    fetch('/api/save', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(data)
    })
      .then(Candle.parseJsonResponse)
      .then(({ ok, json }) => {
        if (!ok || json.status !== 'ok') {
          throw new Error(json.message || t('settings.saveFailed'));
        }
        Candle.showRestartNotice(t('settings.savedRestart'));
        Candle.waitForDeviceAndReload();
      })
      .catch((err) => {
        els.saveBtn.disabled = false;
        Candle.showToast(err.message || t('settings.saveError'), 'error');
      });
  }

  function showResetModal() {
    let modal = document.getElementById('resetModal');
    if (!modal) {
      modal = document.createElement('div');
      modal.id = 'resetModal';
      modal.innerHTML = `
        <div class="modal-backdrop"></div>
        <div class="modal-content">
          <div class="modal-title">${t('settings.resetTitle')}</div>
          <div class="modal-text">${t('settings.resetText')}</div>
          <div class="modal-actions">
            <button id="modalCancel" class="btn btn-secondary" type="button">${t('settings.cancel')}</button>
            <button id="modalConfirm" class="btn" type="button">${t('settings.reset')}</button>
          </div>
        </div>
      `;
      document.body.appendChild(modal);
    }

    modal.style.display = 'flex';
    document.getElementById('modalCancel').onclick = () => {
      modal.style.display = 'none';
    };
    document.getElementById('modalConfirm').onclick = () => {
      modal.style.display = 'none';
      resetToDefaults();
    };
  }

  function resetToDefaults() {
    Candle.showRestartNotice(t('settings.resetRestart'));
    fetch('/api/reset', { method: 'POST' })
      .then(Candle.parseJsonResponse)
      .then(({ ok, json }) => {
        if (!ok || json.status !== 'ok') {
          throw new Error(json.message || t('settings.resetFailed'));
        }
        Candle.waitForDeviceAndReload();
      })
      .catch((err) => {
        document.body.classList.remove('is-restarting');
        Candle.showToast(err.message || t('settings.resetError'), 'error');
      });
  }

  function applySettings(settings) {
    if (els.devname) els.devname.value = settings.devname || '';
    if (els.ssid) els.ssid.value = settings.ssid || '';
    if (els.password) els.password.value = settings.password || '';
    if (els.ntpServer) els.ntpServer.value = settings.ntp_server || '';
    if (els.ntpServer2) els.ntpServer2.value = settings.ntp_server2 || '';
    if (els.ntpTimezone) els.ntpTimezone.value = settings.ntp_timezone || '';
    if (els.lat) els.lat.value = Candle.formatCoordinateValue(settings.lat);
    if (els.lon) els.lon.value = Candle.formatCoordinateValue(settings.lon);
    setManualTimeState(settings);
    updateSaveButtonState();
  }

  function bind() {
    settingsInputs().forEach((input) => {
      input.addEventListener('input', updateSaveButtonState);
      input.addEventListener('change', updateSaveButtonState);
    });

    els.settingsForm?.addEventListener('submit', saveSettings);
    els.resetBtn?.addEventListener('click', showResetModal);
    els.manualTimeBtn?.addEventListener('click', saveManualTime);
    els.manualTime?.addEventListener('input', () => {
      if (!els.manualTime.disabled) {
        els.manualTime.value = normalizeManualTimeInput(els.manualTime.value);
      }
    });
    els.manualTime?.addEventListener('keydown', (event) => {
      if (event.key === 'Enter' && !els.manualTime.disabled) {
        event.preventDefault();
        saveManualTime();
      }
    });
  }

  window.CandleSettings = {
    bind,
    applySettings,
    updateSaveButtonState
  };
}());
