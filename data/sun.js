const canvas = document.getElementById('sunCanvas');
const metaEl = document.getElementById('meta');
const warnEl = document.getElementById('warn');
const statsEl = document.getElementById('stats');
const eventsEl = document.getElementById('events');
const ctx = canvas.getContext('2d');

const MIN_ELEV = -90;
const MAX_ELEV = 90;
let currentData = null;
let locationFromSettings = null;

const TWILIGHT_COLORS = {
  night: 'rgba(6, 10, 24, 0.36)',
  civil: 'rgba(98, 183, 255, 0.20)',
  nautical: 'rgba(57, 121, 197, 0.22)',
  astronomical: 'rgba(22, 53, 113, 0.26)'
};

function normalizeCoord(value) {
  const normalized = String(value || '').trim().replace(',', '.');
  if (!normalized) {
    return null;
  }
  const parsed = Number(normalized);
  return Number.isFinite(parsed) ? parsed : null;
}

async function readJsonResponse(response) {
  const raw = await response.text();
  if (!raw || !raw.trim()) {
    throw new Error('Пустой ответ сервера');
  }

  try {
    return JSON.parse(raw);
  } catch (err) {
    throw new Error('Сервер вернул поврежденный JSON');
  }
}

function minuteToText(minute) {
  if (!Number.isFinite(minute)) {
    return '--:--';
  }
  const m = ((Math.round(minute) % 1440) + 1440) % 1440;
  const h = Math.floor(m / 60);
  const mm = m % 60;
  return String(h).padStart(2, '0') + ':' + String(mm).padStart(2, '0');
}

function deg(value) {
  if (!Number.isFinite(value)) {
    return '--';
  }
  return value.toFixed(1) + '°';
}

function normalizeAzimuth(azimuth) {
  const normalized = Number(azimuth);
  if (!Number.isFinite(normalized)) {
    return null;
  }

  let out = normalized % 360;
  if (out < 0) {
    out += 360;
  }
  return out;
}

function fillAzimuthSpan(ctx2d, xByAzimuth, fromAzimuth, toAzimuth, yTop, yBottom, color) {
  const from = normalizeAzimuth(fromAzimuth);
  const to = normalizeAzimuth(toAzimuth);
  if (from === null || to === null) {
    return;
  }

  ctx2d.fillStyle = color;
  const height = Math.max(0, yBottom - yTop);
  if (height <= 0) {
    return;
  }

  if (Math.abs(from - to) < 1e-6) {
    return;
  }

  if (from <= to) {
    const x1 = xByAzimuth(from);
    const x2 = xByAzimuth(to);
    ctx2d.fillRect(x1, yTop, x2 - x1, height);
    return;
  }

  const xFrom = xByAzimuth(from);
  const x360 = xByAzimuth(360);
  ctx2d.fillRect(xFrom, yTop, x360 - xFrom, height);

  const x0 = xByAzimuth(0);
  const xTo = xByAzimuth(to);
  ctx2d.fillRect(x0, yTop, xTo - x0, height);
}

function modeFromPoint(point) {
  const mode = String(point?.mode || '').toLowerCase();
  if (mode === 'night' || mode === 'civil' || mode === 'nautical' || mode === 'astronomical') {
    return mode;
  }
  return null;
}

function drawTwilightShadows(ctx2d, path, xByAzimuth, chartTop, chartBottom, axisTop, axisBottom) {
  if (!Array.isArray(path) || path.length < 2) {
    return;
  }

  for (let index = 1; index < path.length; index += 1) {
    const prev = path[index - 1];
    const curr = path[index];
    const mode = modeFromPoint(curr) || modeFromPoint(prev);
    if (!mode) {
      continue;
    }

    const color = TWILIGHT_COLORS[mode];
    if (!color) {
      continue;
    }

    fillAzimuthSpan(ctx2d, xByAzimuth, prev.azimuth, curr.azimuth, chartTop, chartBottom, color);
    fillAzimuthSpan(ctx2d, xByAzimuth, prev.azimuth, curr.azimuth, axisTop, axisBottom, color);
  }
}

function draw() {
  const w = canvas.width;
  const h = canvas.height;
  const pad = { left: 74, right: 28, top: 31, bottom: 62 };
  const cw = w - pad.left - pad.right;
  const ch = h - pad.top - pad.bottom;
  const axisBandTop = h - pad.bottom + 8;
  const axisBandBottom = h - 24;

  ctx.clearRect(0, 0, w, h);

  // Background: dark card tone matching --color-card
  ctx.fillStyle = 'rgba(12, 13, 17, 0.92)';
  ctx.fillRect(pad.left, pad.top, cw, ch);

  const grad = ctx.createLinearGradient(0, pad.top, 0, h - pad.bottom);
  grad.addColorStop(0, 'rgba(255, 152, 0, 0.10)');
  grad.addColorStop(0.42, 'rgba(30, 40, 70, 0.10)');
  grad.addColorStop(1, 'rgba(6, 8, 14, 0.30)');
  ctx.fillStyle = grad;
  ctx.fillRect(pad.left, pad.top, cw, ch);

  function yByElev(elev) {
    const t = (elev - MIN_ELEV) / (MAX_ELEV - MIN_ELEV);
    return pad.top + (1 - t) * ch;
  }

  function xByAz(az) {
    return pad.left + (az / 360) * cw;
  }

  function fillBand(fromElev, toElev, color) {
    const y1 = yByElev(fromElev);
    const y2 = yByElev(toElev);
    ctx.fillStyle = color;
    ctx.fillRect(pad.left, Math.min(y1, y2), cw, Math.abs(y2 - y1));
  }

  fillBand(-90, -18, 'rgba(4, 6, 20, 0.60)');         // ночь
  fillBand(-18, -12, 'rgba(18, 28, 72, 0.45)');       // астрономические сумерки
  fillBand(-12, -6, 'rgba(38, 68, 148, 0.38)');       // навигационные сумерки
  fillBand(-6, 0, 'rgba(90, 140, 210, 0.28)');        // гражданские сумерки
  fillBand(0, 90, 'rgba(255, 152, 0, 0.08)');         // день

  // Band labels
  {
    const bands = [
      { from: -90, to: -18, label: 'Ночь',                   color: 'rgba(140, 160, 220, 0.55)' },
      { from: -18, to: -12, label: 'Астрон. сумерки',        color: 'rgba(140, 170, 230, 0.55)' },
      { from: -12, to:  -6, label: 'Навиг. сумерки',         color: 'rgba(160, 190, 240, 0.60)' },
      { from:  -6, to:   0, label: 'Гражд. сумерки',         color: 'rgba(190, 215, 255, 0.65)' },
    ];
    ctx.font = '11px Space Grotesk, sans-serif';
    ctx.textBaseline = 'top';
    bands.forEach(({ from, to, label, color }) => {
      const yTop = Math.min(yByElev(from), yByElev(to));
      const bandH = Math.abs(yByElev(to) - yByElev(from));
      if (bandH < 10) return; // too narrow to label
      ctx.fillStyle = color;
      ctx.fillText(label, pad.left + 6, yTop + 3);
    });
    ctx.textBaseline = 'alphabetic';
  }

  // if (currentData && Array.isArray(currentData.path) && currentData.path.length > 1) {
  //   drawTwilightShadows(
  //     ctx,
  //     currentData.path,
  //     xByAz,
  //     pad.top,
  //     h - pad.bottom,
  //     axisBandTop,
  //     axisBandBottom
  //   );
  // }

  ctx.strokeStyle = 'rgba(255, 152, 0, 0.12)';
  ctx.lineWidth = 1;
  for (let elev = -90; elev <= 90; elev += 30) {
    const y = yByElev(elev);
    ctx.beginPath();
    ctx.moveTo(pad.left, y);
    ctx.lineTo(w - pad.right, y);
    ctx.stroke();

    ctx.fillStyle = 'rgba(224, 210, 194, 0.65)';
    ctx.font = '12px Space Grotesk, sans-serif';
    ctx.fillText(String(elev) + '°', 30, y + 4);
  }

  // Extra -18° twilight boundary line
  {
    const y18 = yByElev(-18);
    ctx.save();
    ctx.strokeStyle = 'rgba(140, 160, 220, 0.40)';
    ctx.lineWidth = 1;
    ctx.setLineDash([3, 4]);
    ctx.beginPath();
    ctx.moveTo(pad.left, y18);
    ctx.lineTo(w - pad.right, y18);
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.fillStyle = 'rgba(160, 185, 240, 0.70)';
    ctx.font = '11px Space Grotesk, sans-serif';
    ctx.fillText('-18°', 30, y18 - 3);
    ctx.restore();
  }

  const azLabels = { 0: 'N', 45: 'NE', 90: 'E', 135: 'SE', 180: 'S', 225: 'SW', 270: 'W', 315: 'NW', 360: 'N' };

  for (let az = 0; az <= 360; az += 45) {
    const x = xByAz(az);
    ctx.beginPath();
    ctx.moveTo(x, pad.top);
    ctx.lineTo(x, h - pad.bottom);
    ctx.stroke();

    const label = azLabels[az] || '';
    ctx.fillStyle = 'rgba(224, 210, 194, 0.65)';
    ctx.font = '12px Space Grotesk, sans-serif';
    ctx.fillText(label, x - ctx.measureText(label).width / 2, h - 20);
  }

  if (!currentData || !Array.isArray(currentData.path) || currentData.path.length < 2) {
    return;
  }

  // Hourly markers (including noon)
  ctx.save();
  ctx.lineWidth = 1;
  ctx.font = '11px Space Grotesk, sans-serif';
  for (let hour = 0; hour < 24; hour++) {
    const minute = hour * 60;
    const pt = currentData.path.reduce((best, p) =>
      Math.abs(p.minute - minute) < Math.abs(best.minute - minute) ? p : best
    );
    const xH = xByAz(pt.azimuth);
    const isNoon = hour === 12;
    ctx.strokeStyle = isNoon ? 'rgba(255, 152, 0, 0.55)' : 'rgba(255, 152, 0, 0.15)';
    ctx.setLineDash(isNoon ? [4, 4] : [2, 5]);
    ctx.beginPath();
    ctx.moveTo(xH, pad.top);
    ctx.lineTo(xH, h - pad.bottom);
    ctx.stroke();
    ctx.setLineDash([]);
    const label = String(hour).padStart(2, '0') + ':00';
    ctx.fillStyle = isNoon ? 'rgba(255, 183, 102, 0.85)' : 'rgba(224, 210, 194, 0.35)';
    ctx.save();
    ctx.translate(xH, pad.top - 16);
    ctx.rotate(-Math.PI / 4);
    ctx.fillText(label, -ctx.measureText(label).width / 2, 0);
    ctx.restore();
  }
  ctx.restore();

  const elevationStats = {
    min: Math.min(...currentData.path.map(p => p.elevation)),
    max: Math.max(...currentData.path.map(p => p.elevation)),
    belowHorizonCount: currentData.path.filter(p => p.elevation < 0).length,
    dayCount: currentData.path.filter(p => p.elevation >= 0).length
  };

  ctx.lineWidth = 2.5;
  ctx.strokeStyle = '#ff9800';
  ctx.shadowBlur = 12;
  ctx.shadowColor = 'rgba(255, 152, 0, 0.55)';

  // Find the 0°/360° wrap index and reorder so the path is one continuous arc
  let wrapIdx = 0;
  {
    let prevAz = normalizeAzimuth(currentData.path[0].azimuth);
    for (let i = 1; i < currentData.path.length; i++) {
      const currAz = normalizeAzimuth(currentData.path[i].azimuth);
      if (currAz !== null && prevAz !== null && Math.abs(currAz - prevAz) > 180) {
        wrapIdx = i;
        break;
      }
      prevAz = currAz;
    }
  }

  const orderedPath = wrapIdx > 0
    ? currentData.path.slice(wrapIdx).concat(currentData.path.slice(0, wrapIdx))
    : currentData.path;

  ctx.beginPath();
  orderedPath.forEach((p, i) => {
    const x = xByAz(p.azimuth);
    const y = yByElev(p.elevation);
    if (i === 0) {
      ctx.moveTo(x, y);
    } else {
      ctx.lineTo(x, y);
    }
  });
  ctx.stroke();

  ctx.shadowBlur = 0;

  const now = currentData.now || {};
  if (Number.isFinite(now.azimuth) && Number.isFinite(now.elevation)) {
    const x = xByAz(now.azimuth);
    const y = yByElev(now.elevation);
    ctx.fillStyle = '#ffb766';
    ctx.beginPath();
    ctx.arc(x, y, 7, 0, Math.PI * 2);
    ctx.fill();

    ctx.strokeStyle = '#ff9800';
    ctx.lineWidth = 1.5;
    ctx.shadowBlur = 10;
    ctx.shadowColor = 'rgba(255, 152, 0, 0.6)';
    ctx.beginPath();
    ctx.arc(x, y, 12, 0, Math.PI * 2);
    ctx.stroke();
    ctx.shadowBlur = 0;
  }
}

function renderStats(data) {
  const now = data.now || {};
  const mode = data.sunMode || '--';

  let statsInfo = [
    ['Текущее время:', minuteToText(now.minuteOfDay)],
    ['Режим:', mode],
    ['Азимут:', deg(now.azimuth)],
    ['Высота:', deg(now.elevation)],
    ['Зенит:', deg(now.zenith)]
  ];

  // Add path statistics
  if (Array.isArray(data.path) && data.path.length > 0) {
    const stats = {
      min: Math.min(...data.path.map(p => p.elevation)).toFixed(1),
      max: Math.max(...data.path.map(p => p.elevation)).toFixed(1)
    };
    statsInfo.push(['Мин. высота:', stats.min + '°']);
    statsInfo.push(['Макс. высота:', stats.max + '°']);
  }

  statsEl.innerHTML = statsInfo
    .map(([k, v]) => '<div class="stat"><span class="k">' + k + '</span><span class="v">' + v + '</span></div>')
    .join('');
}

function renderEvents(data) {
  const ev = data.events || {};

  function durText(minutes) {
    const h = Math.floor(minutes / 60);
    const m = minutes % 60;
    return h + 'ч ' + String(m).padStart(2, '0') + 'м';
  }

  const dayDur = (ev.hasSunrise && ev.hasSunset) ? durText(ev.sunset - ev.sunrise) : null;
  const nightDur = (ev.hasSunrise && ev.hasSunset)
    ? durText(ev.sunrise + (1440 - ev.sunset))
    : null;

  const items = [
    ['Длит. дня:', dayDur || (ev.isPolarDay ? 'полярный день' : 'нет')],
    ['Длит. ночи:', nightDur || (ev.isPolarNight ? 'полярная ночь' : 'нет')],
    ['Восход:', ev.hasSunrise ? minuteToText(ev.sunrise) : 'нет'],
    ['Закат:', ev.hasSunset ? minuteToText(ev.sunset) : 'нет'],
    ['Гражд. сумерки:', ev.hasCivilTwilight ? minuteToText(ev.civilBegin) + ' / ' + minuteToText(ev.civilEnd) : 'нет'],
    ['Навиг. сумерки:', ev.hasNauticalTwilight ? minuteToText(ev.nauticalBegin) + ' / ' + minuteToText(ev.nauticalEnd) : 'нет'],
    ['Астрон. сумерки:', ev.hasAstronomicalTwilight ? minuteToText(ev.astronomicalBegin) + ' / ' + minuteToText(ev.astronomicalEnd) : 'нет'],
    ['Ночь:', ev.hasAstronomicalTwilight ? minuteToText(ev.astronomicalEnd) + ' / ' + minuteToText(ev.astronomicalBegin) : 'нет']
  ];

  if (ev.isPolarDay) {
    items.push(['Полярный статус:', 'полярный день']);
  }
  if (ev.isPolarNight) {
    items.push(['Полярный статус:', 'полярная ночь']);
  }

  eventsEl.innerHTML = items
    .map(([k, v]) => '<div class="stat"><span class="k">' + k + '</span><span class="v">' + v + '</span></div>')
    .join('');
}

function updateMeta(data) {
  const d = data.date || {};
  const offset = data.timezoneOffsetMinutes;
  const location = data.location || {};
  const latText = Number.isFinite(location.lat) ? location.lat.toFixed(6) : '--';
  const lonText = Number.isFinite(location.lon) ? location.lon.toFixed(6) : '--';
  metaEl.textContent =
    'Дата: ' + String(d.year || '--') + '-' + String(d.month || '--').padStart(2, '0') + '-' + String(d.day || '--').padStart(2, '0') +
    ' | UTC offset: ' + (Number.isFinite(offset) ? (offset >= 0 ? '+' : '') + offset + ' min' : '--') +
    ' | lat/lon: ' + latText + ', ' + lonText;

  warnEl.textContent = data.validTime ? '' : 'Внимание: NTP время не подтверждено, расчеты могут быть неточными.';
}

async function loadLocationFromSettingsJson() {
  try {
    const res = await fetch('/settings.json', { cache: 'no-store' });
    const json = await readJsonResponse(res);
    if (!res.ok) {
      throw new Error(json.message || 'settings.json error');
    }

    const lat = normalizeCoord(json?.location?.lat);
    const lon = normalizeCoord(json?.location?.lng);

    if (lat === null || lon === null) {
      throw new Error('location.lat/lng not found in settings.json');
    }

    locationFromSettings = { lat, lon };
  } catch (err) {
    console.error(err);
    locationFromSettings = null;
    warnEl.textContent =
      'Не удалось прочитать координаты из settings.json, использую координаты устройства.';
  }
}

async function refresh() {
  const params = new URLSearchParams();

  if (locationFromSettings) {
    params.set('lat', String(locationFromSettings.lat));
    params.set('lon', String(locationFromSettings.lon));
  }

  const url = '/api/sun' + (params.toString() ? '?' + params.toString() : '');

  metaEl.textContent = 'Обновление...';
  try {
    const res = await fetch(url, { cache: 'no-store' });
    const json = await readJsonResponse(res);

    if (!res.ok || json.status !== 'ok') {
      throw new Error(json.message || 'sun api failed');
    }

    currentData = json;
    updateMeta(json);
    renderStats(json);
    renderEvents(json);
    draw();
  } catch (err) {
    console.error(err);
    metaEl.textContent = 'Ошибка загрузки данных';
    warnEl.textContent = err.message || 'Не удалось получить данные о солнце';
  }
}

window.addEventListener('resize', draw);

(async () => {
  await loadLocationFromSettingsJson();
  await refresh();
  window.setInterval(refresh, 60000);
})();
