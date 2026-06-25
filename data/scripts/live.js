(function () {
  const Candle = window.Candle;
  const { els, state, t } = Candle;
  const kDateRefreshIntervalMs = 10000;
  const MODES = {
    manual: 'manual',
    sun: 'sun',
    time: 'time'
  };

  function formatNtpStatus(ntp) {
    if (!ntp) {
      return { title: t('status.noData'), details: t('status.waitingDevice') };
    }
    if (!ntp.wifiConnected) {
      return { title: t('status.waitingWifi'), details: t('status.staDisconnected') };
    }
    if (!ntp.hasIp) {
      return { title: t('status.waitingIp'), details: t('status.staConnecting') };
    }
    if (ntp.syncInProgress) {
      const seconds = Math.ceil((ntp.syncTimeoutInMs || 0) / 1000);
      return {
        title: t('status.syncing'),
        details: seconds > 0 ? t('status.timeoutIn', { seconds }) : t('status.ntpRequest')
      };
    }
    if (ntp.failureStreak > 0) {
      const seconds = Math.ceil((ntp.nextSyncInMs || 0) / 1000);
      return {
        title: t('status.ntpError'),
        details: seconds > 0 ? t('status.retryIn', { seconds }) : t('status.waitingRetry')
      };
    }
    if (ntp.bootSyncPending) {
      return { title: t('status.waitingStart'), details: t('status.firstSyncPending') };
    }
    return {
      title: t('status.synced'),
      details: `${ntp.server || 'NTP'} / ${ntp.timezone || 'TZ'}`
    };
  }

  function sunStateLabel(mode) {
    switch (String(mode || '').toLowerCase()) {
      case 'day':
        return t('status.sunStateDay');
      case 'civil':
        return t('status.sunStateCivil');
      case 'nautical':
        return t('status.sunStateNautical');
      case 'astronomical':
        return t('status.sunStateAstronomical');
      case 'night':
        return t('status.sunStateNight');
      default:
        return t('status.sunStateUnknown');
    }
  }

  function minuteToTimeInput(minute) {
    const normalized = Number.isFinite(Number(minute)) ? Math.max(0, Math.min(1439, Number(minute))) : 0;
    const hours = Math.floor(normalized / 60);
    const minutes = normalized % 60;
    return `${String(hours).padStart(2, '0')}:${String(minutes).padStart(2, '0')}`;
  }

  function timeInputToMinute(value, fallback) {
    const match = String(value || '').match(/^(\d{1,2}):(\d{2})$/);
    if (!match) {
      return fallback;
    }
    const hours = Number(match[1]);
    const minutes = Number(match[2]);
    if (!Number.isInteger(hours) || !Number.isInteger(minutes) || hours < 0 || hours > 23 || minutes < 0 || minutes > 59) {
      return fallback;
    }
    return hours * 60 + minutes;
  }

  function modeFromSettings(settings) {
    if (settings?.autoTimeMode === true) {
      return MODES.time;
    }
    if (settings?.autoMode !== false) {
      return MODES.sun;
    }
    return MODES.manual;
  }

  function normalizeMode(mode) {
    return mode === MODES.sun || mode === MODES.time ? mode : MODES.manual;
  }

  function modeNeedsTime(mode) {
    return mode === MODES.sun || mode === MODES.time;
  }

  function syncSettingsMode() {
    if (!state.settings) {
      return;
    }
    state.settings.autoMode = state.workMode === MODES.sun;
    state.settings.autoTimeMode = state.workMode === MODES.time;
  }

  function setMode(mode) {
    state.workMode = normalizeMode(mode);
    syncSettingsMode();
    renderModeControls();
  }

  function effectiveUiMode() {
    if (!state.timeSynced && modeNeedsTime(state.workMode)) {
      return MODES.manual;
    }
    return normalizeMode(state.workMode);
  }

  function activeAutoCandleOn(mode) {
    return mode === MODES.time ? state.liveAutoTimeCandleOn : state.liveAutoCandleOn;
  }

  function applyLiveState(json, options = {}) {
    if (typeof json.candleOn === 'boolean' && options.updateManual !== false) {
      state.liveCandleOn = json.candleOn;
    }
    if (typeof json.autoCandleOn === 'boolean') {
      state.liveAutoCandleOn = json.autoCandleOn;
    }
    if (typeof json.autoTimeCandleOn === 'boolean') {
      state.liveAutoTimeCandleOn = json.autoTimeCandleOn;
    }
    state.liveSunMode = json.sunMode || state.liveSunMode || 'unknown';
  }

  function renderTimedLinks() {
    Candle.setTimedNavigationEnabled?.(!!state.timeSynced);
  }

  function renderModeControls() {
    const mode = effectiveUiMode();
    const timeAvailable = !!state.timeSynced;

    [
      [els.manualMode, false, mode === MODES.manual && !!state.liveCandleOn],
      [els.autoMode, !timeAvailable, mode === MODES.sun],
      [els.autoTimeMode, !timeAvailable, mode === MODES.time]
    ].forEach(([input, disabled, checked]) => {
      if (input) {
        input.disabled = disabled;
        input.checked = checked;
      }
    });

    [els.timeOn, els.timeOff].forEach((input) => {
      if (input) {
        input.disabled = !timeAvailable;
      }
    });

    Candle.setText(els.manualModeStateText, t('live.manualMode'));
    Candle.setText(els.autoStateText, t('live.autoSun'));
    Candle.setText(els.autoTimeStateText, t('live.autoTime'));
    renderTimedLinks();
  }

  function updateStatusCards(payload) {
    const wifi = payload?.wifi || {};
    const ntpView = formatNtpStatus(payload?.ntp);
    const mode = effectiveUiMode();

    Candle.setText(els.wifiStaStatus, wifi.staConnected ? t('status.connected') : t('status.disconnected'));
    Candle.setText(
      els.wifiStaDetails,
      wifi.staConnected
        ? `${wifi.ssid || t('status.hiddenSsid')} / ${wifi.ip || t('status.noIp')} / ${wifi.rssi || 0} dBm`
        : (wifi.ssid ? t('status.networkPrefix', { ssid: wifi.ssid }) : t('status.networkMissing'))
    );

    Candle.setText(els.wifiApStatus, wifi.setupApActive ? t('status.active') : t('status.disabled'));
    Candle.setText(
      els.wifiApDetails,
      wifi.setupApActive ? `${wifi.setupSsid || 'candle-setup'} / ${wifi.setupIp || '192.168.4.1'}` : t('status.staWithoutAp')
    );

    Candle.setText(els.ntpStatus, ntpView.title);
    Candle.setText(els.ntpDetails, ntpView.details);

    Candle.setText(els.autoModeStatus, mode === MODES.manual ? t('status.manual') : t('status.auto'));
    if (mode === MODES.sun) {
      const candleState = state.liveAutoCandleOn ? t('status.candleAutoOn') : t('status.candleAutoOff');
      Candle.setText(els.autoModeDetails, `${sunStateLabel(state.liveSunMode)} / ${candleState}`);
    } else if (mode === MODES.time) {
      const candleState = state.liveAutoTimeCandleOn ? t('status.candleAutoOn') : t('status.candleAutoOff');
      Candle.setText(els.autoModeDetails, `${t('live.autoTime')} / ${candleState}`);
    } else {
      Candle.setText(els.autoModeDetails, state.liveCandleOn ? t('status.candleManualOn') : t('status.candleManualOff'));
    }
  }

  function updateDate(dateText, isValid = true, ntp = null) {
    if (!els.status) {
      return;
    }
    els.status.textContent = dateText || t('status.timePlaceholder');
    els.status.classList.toggle('time-pill--muted', !isValid);
    els.status.title = isValid ? '' : formatNtpStatus(ntp).details;
  }

  function updateUI() {
    const brightness = Candle.normalizeBrightnessPercent(els.brightness?.value ?? state.settings?.brightness ?? 0);
    if (brightness > 0) {
      state.lastManualBrightness = brightness;
    }
    Candle.setText(els.brightnessValue, `${brightness}%`);
    renderModeControls();
    updateStatusCards(state.datePayload);
  }

  function sendControlState() {
    if (!els.brightness) {
      return Promise.resolve(null);
    }

    const value = Candle.normalizeBrightnessPercent(els.brightness.value);
    const mode = effectiveUiMode();
    const autoMode = mode === MODES.sun;
    const autoTimeMode = mode === MODES.time;
    const candleOn = mode === MODES.manual ? state.liveCandleOn : activeAutoCandleOn(mode);

    const query = new URLSearchParams({
      value: String(value),
      candleOn: candleOn ? '1' : '0',
      autoMode: autoMode ? '1' : '0',
      autoTimeMode: autoTimeMode ? '1' : '0',
      timeOnMinute: String(timeInputToMinute(els.timeOn?.value, state.settings?.timeOnMinute ?? 18 * 60)),
      timeOffMinute: String(timeInputToMinute(els.timeOff?.value, state.settings?.timeOffMinute ?? 23 * 60))
    });

    return fetch(`/api/brightness?${query.toString()}`, { method: 'POST' })
      .then(Candle.parseJsonResponse)
      .then(({ ok, json }) => {
        if (!ok || json.status !== 'ok') {
          throw new Error(json.message || t('live.brightnessApplyError'));
        }

        applyLiveState(json);

        if (state.settings) {
          syncSettingsMode();
          state.settings.timeOnMinute = timeInputToMinute(els.timeOn?.value, state.settings.timeOnMinute);
          state.settings.timeOffMinute = timeInputToMinute(els.timeOff?.value, state.settings.timeOffMinute);
          state.settings.candleOn = state.liveCandleOn;
          state.settings.brightness = value;
        }
        updateUI();
        return json;
      })
      .catch((err) => {
        Candle.showToast(err.message || t('live.brightnessError'), 'error');
        return null;
      });
  }

  function fetchDate() {
    return fetch('/api/date', { cache: 'no-store' })
      .then(Candle.parseJsonResponse)
      .then(({ ok, json }) => {
        if (!ok) {
          throw new Error(json.message || t('live.statusError'));
        }

        state.datePayload = json;
        updateDate(json.date, json.valid !== false, json.ntp);
        state.timeSynced = json.valid !== false && json.ntp?.validTime !== false;
        applyLiveState(json, { updateManual: effectiveUiMode() !== MODES.manual });
        renderModeControls();
        updateStatusCards(json);
        return json;
      })
      .catch((err) => {
        Candle.showToast(err.message || t('live.statusError'), 'error');
        return null;
      });
  }

  function startDateUpdates() {
    if (!els.status && !els.wifiStaStatus && !els.ntpStatus) {
      return;
    }

    fetchDate();
    if (state.dateRefreshTimer) {
      clearInterval(state.dateRefreshTimer);
    }
    state.dateRefreshTimer = setInterval(fetchDate, kDateRefreshIntervalMs);
  }

  function applySettings(settings) {
    state.timeSynced = settings.validTime === true;
    state.workMode = modeFromSettings(settings);
    if (els.timeOn) {
      els.timeOn.value = minuteToTimeInput(settings.timeOnMinute ?? 18 * 60);
    }
    if (els.timeOff) {
      els.timeOff.value = minuteToTimeInput(settings.timeOffMinute ?? 23 * 60);
    }
    if (els.brightness) {
      els.brightness.value = settings.brightness != null ? Candle.normalizeBrightnessPercent(settings.brightness) : 0;
    }

    state.liveCandleOn = settings.candleOn != null ? !!settings.candleOn : (settings.brightness ?? 0) > 0;
    state.liveAutoCandleOn = settings.autoCandleOn != null ? !!settings.autoCandleOn : state.liveCandleOn;
    state.liveAutoTimeCandleOn = settings.autoTimeCandleOn != null ? !!settings.autoTimeCandleOn : false;
    state.liveSunMode = settings.sunMode || 'unknown';
    if ((settings.brightness ?? 0) > 0) {
      state.lastManualBrightness = settings.brightness;
    }

    updateUI();
    startDateUpdates();
  }

  function bindModeToggle(input, mode) {
    input?.addEventListener('change', () => {
      const requestedChecked = !!input.checked;
      if (mode === MODES.manual) {
        state.liveCandleOn = requestedChecked;
        setMode(MODES.manual);
      } else if (requestedChecked) {
        setMode(mode);
      } else {
        setMode(MODES.manual);
        state.liveCandleOn = false;
      }
      updateUI();
      sendControlState();
    });
  }

  function bind() {
    bindModeToggle(els.manualMode, MODES.manual);
    bindModeToggle(els.autoMode, MODES.sun);
    bindModeToggle(els.autoTimeMode, MODES.time);

    if (els.brightness) {
      els.brightness.addEventListener('input', updateUI);
      els.brightness.addEventListener('change', sendControlState);
    }
    els.timeOn?.addEventListener('change', sendControlState);
    els.timeOff?.addEventListener('change', sendControlState);
  }

  window.CandleLive = {
    bind,
    applySettings,
    updateStatusCards,
    fetchDate
  };
}());
