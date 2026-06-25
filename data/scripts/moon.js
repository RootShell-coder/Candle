'use strict';

const SYNODIC = 29.53058770576;

const MOON_DEFAULT_LOCALE = {
  phases: [
    'Новолуние',
    'Растущий серп',
    'Первая четверть',
    'Растущая луна',
    'Полнолуние',
    'Убывающая луна',
    'Последняя четверть',
    'Убывающий серп'
  ],
  stats: {
    phase: 'Фаза',
    age: 'Возраст луны',
    illumination: 'Освещенность диска',
    nextNewMoon: 'До новолуния'
  },
  led: {
    enabled: 'Включено',
    disabled: 'Выключено',
    pinMissing: 'Пин не настроен',
    calculatedBrightness: 'Расчетная яркость: {calculated}% / выход: {current}%',
    saveError: 'Не удалось сохранить настройки лунного LED',
    genericSaveError: 'Ошибка сохранения'
  },
  errors: {
    emptyResponse: 'Пустой ответ сервера',
    invalidJson: 'Сервер вернул неверный JSON: {preview}',
    noMoonData: 'Нет данных о луне'
  },
  units: {
    days: 'д'
  }
};

function moonText(path, params = {}) {
  const custom = window.CandleMoonLocale || {};
  const customValue = path.split('.').reduce((node, key) => node && node[key], custom);
  const defaultValue = path.split('.').reduce((node, key) => node && node[key], MOON_DEFAULT_LOCALE);
  const template = typeof customValue === 'string'
    ? customValue
    : (typeof defaultValue === 'string' ? defaultValue : path);
  return Object.keys(params).reduce(
    (out, key) => out.replaceAll(`{${key}}`, String(params[key])),
    template
  );
}

function moonPhaseName(index, fallback = '--') {
  const customPhases = Array.isArray(window.CandleMoonLocale?.phases) ? window.CandleMoonLocale.phases : null;
  const phases = customPhases || MOON_DEFAULT_LOCALE.phases;
  return phases[index] || fallback;
}

function applyStaticLocale() {
  document.querySelectorAll('[data-moon-text]').forEach((element) => {
    element.textContent = moonText(element.dataset.moonText);
  });
}

let currentMoonData = null;
let currentSettings = null;

function clampPercent(value) {
  const parsed = Number.parseInt(value, 10);
  if (!Number.isFinite(parsed) || parsed <= 0) {
    return 0;
  }
  return Math.min(100, parsed);
}

function clampHue(value) {
  const parsed = Number.parseInt(value, 10);
  if (!Number.isFinite(parsed)) {
    return 42;
  }
  return Math.max(0, Math.min(360, parsed));
}

function hueToCss(hue, lightness = 68) {
  return `hsl(${clampHue(hue)} 88% ${lightness}%)`;
}

function drawMoonDisc(ctx, cx, cy, r, phase, lightColor = '#d4c4a8') {
  const dark = '#0d0e13';
  const craters = [
    { dx: 0.22, dy: -0.18, r: 0.09 },
    { dx: -0.28, dy: 0.14, r: 0.07 },
    { dx: 0.05, dy: 0.30, r: 0.11 },
    { dx: -0.12, dy: -0.32, r: 0.06 },
    { dx: 0.30, dy: 0.08, r: 0.05 }
  ];

  const pi = Math.PI;
  const pi2 = 2 * pi;

  if (phase > 0.03 && phase < 0.97) {
    const glowR = r * 1.35;
    const glow = ctx.createRadialGradient(cx, cy, r * 0.82, cx, cy, glowR);
    const alpha = Math.sin(pi * phase) * 0.14;
    glow.addColorStop(0, `rgba(220, 200, 160, ${alpha})`);
    glow.addColorStop(1, 'rgba(0,0,0,0)');
    ctx.beginPath();
    ctx.arc(cx, cy, glowR, 0, pi2);
    ctx.fillStyle = glow;
    ctx.fill();
  }

  ctx.beginPath();
  ctx.arc(cx, cy, r, 0, pi2);
  ctx.fillStyle = dark;
  ctx.fill();

  const isNew = phase < 0.015 || phase > 0.985;
  const isFull = Math.abs(phase - 0.5) < 0.015;

  if (!isNew) {
    ctx.save();
    ctx.beginPath();
    ctx.arc(cx, cy, r, 0, pi2);
    ctx.clip();
    ctx.beginPath();

    if (isFull) {
      ctx.arc(cx, cy, r, 0, pi2);
    } else if (phase < 0.5) {
      const rx = r * Math.cos(pi2 * phase);
      ctx.arc(cx, cy, r, -pi / 2, pi / 2, false);
      ctx.ellipse(cx, cy, Math.abs(rx), r, 0, pi / 2, -pi / 2, rx >= 0);
    } else {
      const rx = r * Math.cos(pi2 * (phase - 0.5));
      ctx.arc(cx, cy, r, -pi / 2, pi / 2, true);
      ctx.ellipse(cx, cy, Math.abs(rx), r, 0, pi / 2, -pi / 2, rx >= 0);
    }

    ctx.closePath();
    ctx.fillStyle = lightColor;
    ctx.fill();

    if (r > 30) {
      ctx.globalAlpha = 0.10;
      craters.forEach((c) => {
        ctx.beginPath();
        ctx.arc(cx + c.dx * r, cy + c.dy * r, c.r * r, 0, pi2);
        ctx.fillStyle = 'rgba(0,0,0,0.6)';
        ctx.fill();
      });
      ctx.globalAlpha = 1;
    }

    ctx.restore();
  }

  ctx.beginPath();
  ctx.arc(cx, cy, r, 0, pi2);
  ctx.strokeStyle = 'rgba(180, 160, 130, 0.20)';
  ctx.lineWidth = Math.max(1, r * 0.012);
  ctx.stroke();
}

function drawStars(ctx, w, h) {
  const pts = [
    [0.08, 0.12], [0.18, 0.55], [0.31, 0.08], [0.44, 0.78],
    [0.57, 0.22], [0.63, 0.65], [0.72, 0.40], [0.82, 0.15],
    [0.90, 0.72], [0.95, 0.35], [0.14, 0.88], [0.38, 0.47],
    [0.50, 0.93], [0.76, 0.85], [0.25, 0.30]
  ];
  const sizes = [0.8, 0.5, 1.0, 0.6, 0.9, 0.4, 0.7, 1.1, 0.5, 0.8, 0.6, 0.9, 0.4, 0.7, 1.0];
  const alphas = [0.35, 0.50, 0.28, 0.60, 0.42, 0.55, 0.38, 0.65, 0.30, 0.48, 0.58, 0.33, 0.62, 0.45, 0.52];

  pts.forEach((pt, index) => {
    ctx.beginPath();
    ctx.arc(pt[0] * w, pt[1] * h, sizes[index], 0, Math.PI * 2);
    ctx.fillStyle = `rgba(255,255,255,${alphas[index]})`;
    ctx.fill();
  });
}

function buildCycleStrip(container, activePhaseIndex) {
  container.innerHTML = '';

  for (let i = 0; i < 8; i += 1) {
    const item = document.createElement('div');
    item.className = `cycle-item${i === activePhaseIndex ? ' active' : ''}`;

    const cvs = document.createElement('canvas');
    cvs.width = 56;
    cvs.height = 56;
    cvs.className = 'cycle-canvas';
    const cc = cvs.getContext('2d');
    cc.fillStyle = '#0d0e13';
    cc.fillRect(0, 0, 56, 56);
    drawMoonDisc(cc, 28, 28, 22, i / 8);

    const lbl = document.createElement('span');
    lbl.className = 'cycle-label';
    lbl.textContent = moonPhaseName(i);

    item.appendChild(cvs);
    item.appendChild(lbl);
    container.appendChild(item);
  }
}

function renderMoon(data) {
  const canvas = document.getElementById('moonCanvas');
  const infoEl = document.getElementById('moonInfo');
  const cycleEl = document.getElementById('moonCycle');
  const ctx = canvas.getContext('2d');
  const w = canvas.width;
  const h = canvas.height;

  const ageDays = Number(data.ageDays) || 0;
  const illumination = Math.max(0, Math.min(1, Number(data.illumination) || 0));
  const phaseIndex = Number(data.phase) || 0;
  const phaseName = moonPhaseName(phaseIndex, data.phaseName || '--');
  const phase = ageDays / SYNODIC;
  const daysLeft = SYNODIC - ageDays;
  const pct = Math.round(illumination * 100);

  ctx.fillStyle = '#0a0b0f';
  ctx.fillRect(0, 0, w, h);
  drawStars(ctx, w, h);
  const hue = clampHue(currentSettings?.moonLed?.hue ?? currentMoonData?.moonLed?.hue ?? 42);
  drawMoonDisc(ctx, w / 2, h / 2, Math.round(Math.min(w, h) * 0.36), phase, hueToCss(hue, 68));

  ctx.font = `bold ${Math.round(w * 0.048)}px "Segoe UI", Arial, sans-serif`;
  ctx.textAlign = 'center';
  ctx.fillStyle = 'rgba(230, 210, 180, 0.82)';
  ctx.fillText(phaseName, w / 2, h - Math.round(h * 0.07));

  infoEl.innerHTML =
    `<div class="moon-stat"><span class="moon-stat-k">${moonText('stats.phase')}</span><span class="moon-stat-v">${phaseName}</span></div>` +
    `<div class="moon-stat"><span class="moon-stat-k">${moonText('stats.age')}</span><span class="moon-stat-v">${ageDays.toFixed(2)} ${moonText('units.days')}</span></div>` +
    `<div class="moon-stat"><span class="moon-stat-k">${moonText('stats.illumination')}</span><span class="moon-stat-v">${pct}%</span></div>` +
    `<div class="moon-stat"><span class="moon-stat-k">${moonText('stats.nextNewMoon')}</span><span class="moon-stat-v">${daysLeft.toFixed(1)} ${moonText('units.days')}</span></div>`;

  buildCycleStrip(cycleEl, phaseIndex);
}

function calculatedMoonLedBrightness() {
  const limit = clampPercent(document.getElementById('moonLedMaxBrightness')?.value ?? 0);
  const illumination = Math.max(0, Math.min(1, Number(currentMoonData?.illumination) || 0));
  return Math.round(illumination * limit);
}

function renderMoonLedControls(settings, moonData) {
  currentSettings = settings || currentSettings || {};
  currentMoonData = moonData || currentMoonData;

  const cfg = currentSettings.moonLed || {};
  const enabledEl = document.getElementById('moonLedEnabled');
  const sliderEl = document.getElementById('moonLedMaxBrightness');
  const valueEl = document.getElementById('moonLedMaxBrightnessValue');
  const hueEl = document.getElementById('moonLedHue');
  const hueValueEl = document.getElementById('moonLedHueValue');
  const swatchEl = document.getElementById('moonLedSwatch');
  const colorTextEl = document.getElementById('moonLedColorText');
  const detailsEl = document.getElementById('moonLedDetails');
  const stateEl = document.getElementById('moonLedState');
  const maxBrightness = clampPercent(cfg.maxBrightness ?? 25);
  const hue = clampHue(cfg.hue ?? 42);

  if (enabledEl) enabledEl.checked = cfg.enabled === true;
  if (sliderEl && document.activeElement !== sliderEl) sliderEl.value = String(maxBrightness);
  if (hueEl && document.activeElement !== hueEl) hueEl.value = String(hue);
  if (valueEl) valueEl.textContent = `${clampPercent(sliderEl?.value ?? maxBrightness)}%`;
  if (hueValueEl) hueValueEl.textContent = moonText('led.hueValue', { hue: clampHue(hueEl?.value ?? hue) });
  if (swatchEl) swatchEl.style.background = hueToCss(hueEl?.value ?? hue, 56);
  if (colorTextEl) colorTextEl.textContent = moonText('led.colorValue', { hue: clampHue(hueEl?.value ?? hue) });

  const calculated = calculatedMoonLedBrightness();
  const current = clampPercent(currentMoonData?.moonLed?.currentBrightness ?? cfg.currentBrightness ?? calculated);
  const hardware = currentMoonData?.moonLed?.hardwareEnabled === true || cfg.hardwareEnabled === true;

  if (detailsEl) {
    detailsEl.textContent = moonText('led.calculatedBrightness', { calculated, current });
  }
  if (stateEl) {
    stateEl.textContent = cfg.enabled
      ? (hardware ? moonText('led.enabled') : moonText('led.pinMissing'))
      : moonText('led.disabled');
  }
}

async function readJsonResponse(response) {
  const raw = await response.text();
  if (!raw || !raw.trim()) {
    throw new Error(moonText('errors.emptyResponse'));
  }

  try {
    return JSON.parse(raw);
  } catch (err) {
    const preview = raw.slice(0, 180).replace(/[\u0000-\u001f]/g, ' ');
    throw new Error(moonText('errors.invalidJson', { preview }));
  }
}

async function loadMoonData() {
  const resp = await fetch('/api/moon', { cache: 'no-store' });
  const data = await readJsonResponse(resp);
  if (!resp.ok || data.status !== 'ok') {
    throw new Error(data.message || `HTTP ${resp.status}`);
  }
  currentMoonData = data;
  return data;
}

async function saveMoonLedSettings() {
  if (!currentSettings) {
    return;
  }

  const enabled = document.getElementById('moonLedEnabled')?.checked === true;
  const maxBrightness = clampPercent(document.getElementById('moonLedMaxBrightness')?.value ?? 0);
  const hue = clampHue(document.getElementById('moonLedHue')?.value ?? 42);
  const query = new URLSearchParams({
    enabled: enabled ? '1' : '0',
    brightness: String(maxBrightness),
    hue: String(hue)
  });
  const resp = await fetch(`/api/moon-led?${query.toString()}`, { method: 'POST' });
  const json = await readJsonResponse(resp);
  if (!resp.ok || json.status !== 'ok') {
    throw new Error(json.message || moonText('led.saveError'));
  }

  currentSettings.moonLed = {
    ...(json.moonLed || currentSettings.moonLed || {}),
    enabled,
    maxBrightness,
    hue
  };
  if (currentMoonData) {
    currentMoonData.moonLed = json.moonLed || currentMoonData.moonLed;
  }
  renderMoonLedControls(currentSettings, currentMoonData);
}

function bindMoonLedControls() {
  const enabledEl = document.getElementById('moonLedEnabled');
  const sliderEl = document.getElementById('moonLedMaxBrightness');
  const hueEl = document.getElementById('moonLedHue');
  let saveTimer = null;

  const scheduleSave = () => {
    window.clearTimeout(saveTimer);
    if (currentSettings) {
      currentSettings.moonLed = {
        ...(currentSettings.moonLed || {}),
        enabled: enabledEl?.checked === true,
        maxBrightness: clampPercent(sliderEl?.value ?? 0),
        hue: clampHue(hueEl?.value ?? 42)
      };
    }
    renderMoonLedControls(currentSettings, currentMoonData);
    if (currentMoonData) {
      renderMoon(currentMoonData);
    }
    saveTimer = window.setTimeout(() => {
      saveMoonLedSettings().catch((err) => {
        const detailsEl = document.getElementById('moonLedDetails');
        if (detailsEl) detailsEl.textContent = err.message || moonText('led.genericSaveError');
      });
    }, 350);
  };

  enabledEl?.addEventListener('change', scheduleSave);
  sliderEl?.addEventListener('input', () => renderMoonLedControls(currentSettings, currentMoonData));
  sliderEl?.addEventListener('change', scheduleSave);
  hueEl?.addEventListener('input', () => {
    if (currentSettings) {
      currentSettings.moonLed = {
        ...(currentSettings.moonLed || {}),
        hue: clampHue(hueEl.value)
      };
    }
    renderMoonLedControls(currentSettings, currentMoonData);
    if (currentMoonData) {
      renderMoon(currentMoonData);
    }
  });
  hueEl?.addEventListener('change', scheduleSave);
}

async function init() {
  try {
    applyStaticLocale();
    bindMoonLedControls();
    const settingsPromise = window.Candle ? window.Candle.loadSettings() : Promise.resolve({});
    const [settings, moonData] = await Promise.all([settingsPromise, loadMoonData()]);
    currentSettings = settings;
    renderMoon(moonData);
    renderMoonLedControls(settings, moonData);
  } catch (err) {
    document.getElementById('moonInfo').textContent = err.message || moonText('errors.noMoonData');
  }
}

init();
