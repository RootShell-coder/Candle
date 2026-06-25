const canvas = document.getElementById('sunCanvas');
const metaEl = document.getElementById('meta');
const warnEl = document.getElementById('warn');
const statsEl = document.getElementById('stats');
const eventsEl = document.getElementById('events');
const ctx = canvas.getContext('2d');

const MIN_ELEV = -90;
const MAX_ELEV = 90;
const DAY_MINUTES = 1440;
const GOLDEN_HOUR_MINUTES = 60;
const SUN_DEFAULT_LOCALE = {
  errors: {
    emptyResponse: 'Пустой ответ сервера',
    invalidJson: 'Сервер вернул неверный JSON: ',
    settingsJson: 'Ошибка settings.json',
    locationMissing: 'В settings.json не найдены location.lat/lng',
    settingsCoordinates: 'Не удалось прочитать координаты из settings.json; используются координаты устройства.',
    sunApi: 'Ошибка API солнца',
    dataLoad: 'Не удалось загрузить данные',
    sunFetch: 'Не удалось получить данные солнца'
  },
  meta: {
    date: 'Дата',
    utcOffset: 'Смещение UTC',
    latLon: 'широта/долгота',
    updating: 'Обновление...',
    ntpWarning: 'Внимание: время NTP не подтверждено; расчеты могут быть неточными.'
  },
  bands: {
    night: 'Ночь',
    astronomical: 'Астрономические сумерки',
    nautical: 'Навигационные сумерки',
    civil: 'Гражданские сумерки',
    sunset: 'Закатные сумерки',
    goldenHour: 'Золотой час'
  },
  stats: {
    currentTime: 'Текущее время:',
    mode: 'Режим:',
    azimuth: 'Азимут:',
    elevation: 'Высота:',
    zenith: 'Зенит:',
    minElevation: 'Мин. высота:',
    maxElevation: 'Макс. высота:'
  },
  events: {
    dayLength: 'Длит. дня:',
    nightLength: 'Длит. ночи:',
    sunsetToSunrise: 'Длит. ночи:',
    sunrise: 'Восход:',
    sunset: 'Закат:',
    goldenBeforeSunset: 'До заката:',
    goldenAfterSunrise: 'После рассвета:',
    civil: 'Закатные сумерки:',
    nautical: 'Навиг. сумерки:',
    astronomical: 'Астрон. сумерки:',
    night: 'Ночь:',
    polarStatus: 'Полярный статус:',
    polarDay: 'полярный день',
    polarNight: 'полярная ночь',
    none: 'нет'
  },
  groups: {
    current: 'Сейчас',
    dayBounds: 'Диапазон дня',
    durations: 'Длительность',
    sunEvents: 'События солнца',
    goldenHour: 'Золотой час',
    twilight: 'Сумерки'
  },
  units: {
    hour: 'ч',
    minute: 'м'
  }
};
let currentData = null;
let locationFromSettings = null;

function sunText(path) {
  const custom = window.CandleSunLocale || {};
  const value = path.split('.').reduce((node, key) => node && node[key], custom);
  if (typeof value === 'string') {
    return value;
  }
  return path.split('.').reduce((node, key) => node && node[key], SUN_DEFAULT_LOCALE) || path;
}

function applyStaticLocale() {
  if (window.CandleSunLocale?.pageTitle) {
    document.title = window.CandleSunLocale.pageTitle;
  }

  const header = document.querySelector('.sun-page h1');
  if (header && window.CandleSunLocale?.heading) {
    header.textContent = window.CandleSunLocale.heading;
  }

  const subtitle = document.querySelector('.sun-page .app-subtitle');
  if (subtitle && window.CandleSunLocale?.subtitle) {
    subtitle.textContent = window.CandleSunLocale.subtitle;
  }

  const panelLabel = document.querySelector('.sun-page .panel-label');
  if (panelLabel && window.CandleSunLocale?.panelLabel) {
    panelLabel.textContent = window.CandleSunLocale.panelLabel;
  }

  const panelTitle = document.querySelector('.sun-page .panel-head h2');
  if (panelTitle && window.CandleSunLocale?.panelTitle) {
    panelTitle.textContent = window.CandleSunLocale.panelTitle;
  }
}

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
    throw new Error(sunText('errors.emptyResponse'));
  }

  try {
    return JSON.parse(raw);
  } catch (err) {
    const preview = raw.slice(0, 180).replace(/[\u0000-\u001f]/g, ' ');
    throw new Error(sunText('errors.invalidJson') + preview);
  }
}

function minuteToText(minute) {
  if (!Number.isFinite(minute)) {
    return '--:--';
  }
  const m = ((Math.round(minute) % DAY_MINUTES) + DAY_MINUTES) % DAY_MINUTES;
  const h = Math.floor(m / 60);
  const mm = m % 60;
  return String(h).padStart(2, '0') + ':' + String(mm).padStart(2, '0');
}

function emptyTimeRange() {
  return '--:-- - --:--';
}

function statHtml(key, value) {
  return '<div class="stat"><span class="k">' + key + '</span><span class="v">' + value + '</span></div>';
}

function statGroupHtml(title, items) {
  return '<section class="stat-group"><h3 class="stat-group-title">' + title + '</h3><div class="stat-group-grid">' +
    items.map(([key, value]) => statHtml(key, value)).join('') +
    '</div></section>';
}

function deg(value) {
  if (!Number.isFinite(value)) {
    return '--';
  }
  return value.toFixed(1) + '\u00B0';
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

function normalizedSunPathPoints(path) {
  if (!Array.isArray(path)) {
    return [];
  }

  return path
    .map((point) => {
      const minute = Number(point?.minute);
      const azimuth = normalizeAzimuth(point?.azimuth);
      const elevation = Number(point?.elevation);

      if (!Number.isFinite(minute) || azimuth === null || !Number.isFinite(elevation)) {
        return null;
      }

      return { minute, azimuth, elevation };
    })
    .filter(Boolean)
    .sort((a, b) => a.minute - b.minute);
}

function sunPointToCanvasPoint(point, xByAzimuth, yByElevation) {
  return {
    minute: point.minute,
    azimuth: point.azimuth,
    elevation: point.elevation,
    x: xByAzimuth(point.azimuth),
    y: yByElevation(point.elevation)
  };
}

function crossesAzimuthEdge(prev, curr) {
  return (prev.azimuth > 270 && curr.azimuth < 90) ||
    (prev.azimuth < 90 && curr.azimuth > 270);
}

function circularAzimuthDelta(a, b) {
  const delta = Math.abs(a - b);
  return Math.min(delta, 360 - delta);
}

function shouldCloseDailyPath(last, first) {
  if (!last || !first || crossesAzimuthEdge(last, first)) {
    return false;
  }

  const wrappedMinuteGap = first.minute + DAY_MINUTES - last.minute;
  const azimuthGap = circularAzimuthDelta(last.azimuth, first.azimuth);
  const elevationGap = Math.abs(last.elevation - first.elevation);

  return wrappedMinuteGap > 0 &&
    wrappedMinuteGap <= 20 &&
    azimuthGap <= 20 &&
    elevationGap <= 5;
}

function buildSunPathSegments(path, xByAzimuth, yByElevation) {
  const points = normalizedSunPathPoints(path)
    .map((point) => sunPointToCanvasPoint(point, xByAzimuth, yByElevation));

  if (points.length === 0) {
    return [];
  }

  const segments = [[points[0]]];

  for (let index = 1; index < points.length; index += 1) {
    const prev = points[index - 1];
    const curr = points[index];
    const active = segments[segments.length - 1];

    if (crossesAzimuthEdge(prev, curr)) {
      segments.push([curr]);
    } else {
      active.push(curr);
    }
  }

  const first = points[0];
  const last = points[points.length - 1];
  if (shouldCloseDailyPath(last, first)) {
    segments[segments.length - 1].push(first);
  }

  return segments.filter((segment) => segment.length > 1);
}

function drawSunPathSegments(ctx2d, segments, chartLeft, chartTop, chartWidth, chartHeight) {
  if (!Array.isArray(segments) || segments.length === 0) {
    return;
  }

  ctx2d.save();
  ctx2d.beginPath();
  ctx2d.rect(chartLeft, chartTop, chartWidth, chartHeight);
  ctx2d.clip();

  ctx2d.lineWidth = 1;
  ctx2d.strokeStyle = '#ff9800';
  ctx2d.lineJoin = 'miter';
  ctx2d.lineCap = 'butt';
  ctx2d.shadowBlur = 0;

  segments.forEach((segment) => {
    ctx2d.beginPath();
    ctx2d.moveTo(segment[0].x, segment[0].y);
    for (let index = 1; index < segment.length; index += 1) {
      ctx2d.lineTo(segment[index].x, segment[index].y);
    }
    ctx2d.stroke();
  });

  ctx2d.restore();
}

function splitMinuteRange(from, to) {
  const start = ((from % DAY_MINUTES) + DAY_MINUTES) % DAY_MINUTES;
  const end = ((to % DAY_MINUTES) + DAY_MINUTES) % DAY_MINUTES;
  if (start === end) {
    return [];
  }
  if (start < end) {
    return [{ start, end }];
  }
  return [
    { start, end: DAY_MINUTES },
    { start: 0, end }
  ];
}

function interpolateSegmentPoint(from, to, minute) {
  const span = to.minute - from.minute;
  const t = span === 0 ? 0 : (minute - from.minute) / span;
  return {
    minute,
    x: from.x + (to.x - from.x) * t,
    y: from.y + (to.y - from.y) * t
  };
}

function sunPathPointAtMinute(segments, minute) {
  const normalized = ((minute % DAY_MINUTES) + DAY_MINUTES) % DAY_MINUTES;
  let closest = null;
  let closestDistance = Infinity;

  for (const segment of segments) {
    for (let index = 1; index < segment.length; index += 1) {
      const from = segment[index - 1];
      const to = segment[index];
      if (to.minute < from.minute) {
        continue;
      }
      if (normalized >= from.minute && normalized <= to.minute) {
        return interpolateSegmentPoint(from, to, normalized);
      }
    }

    for (const point of segment) {
      const distance = Math.abs(point.minute - normalized);
      const wrappedDistance = Math.min(distance, DAY_MINUTES - distance);
      if (wrappedDistance < closestDistance) {
        closest = point;
        closestDistance = wrappedDistance;
      }
    }
  }

  return closest;
}

function drawGoldenHourRanges(ctx2d, segments, events) {
  if (!events?.hasSunrise || !events?.hasSunset || !Array.isArray(segments) || segments.length === 0) {
    return;
  }

  const ranges = [
    { from: Number(events.sunrise), to: Number(events.sunrise) + GOLDEN_HOUR_MINUTES },
    { from: Number(events.sunset) - GOLDEN_HOUR_MINUTES, to: Number(events.sunset) }
  ].filter(({ from, to }) => Number.isFinite(from) && Number.isFinite(to));

  ctx2d.save();
  ctx2d.lineWidth = 5;
  ctx2d.strokeStyle = 'rgba(255, 211, 106, 0.92)';
  ctx2d.lineCap = 'round';
  ctx2d.lineJoin = 'round';
  ctx2d.shadowBlur = 11;
  ctx2d.shadowColor = 'rgba(255, 180, 64, 0.55)';

  ranges.forEach((range) => {
    splitMinuteRange(range.from, range.to).forEach((part) => {
      segments.forEach((segment) => {
        for (let index = 1; index < segment.length; index += 1) {
          const from = segment[index - 1];
          const to = segment[index];
          if (to.minute < from.minute) {
            continue;
          }

          const start = Math.max(from.minute, part.start);
          const end = Math.min(to.minute, part.end);
          if (end <= start) {
            continue;
          }

          const startPoint = interpolateSegmentPoint(from, to, start);
          const endPoint = interpolateSegmentPoint(from, to, end);
          ctx2d.beginPath();
          ctx2d.moveTo(startPoint.x, startPoint.y);
          ctx2d.lineTo(endPoint.x, endPoint.y);
          ctx2d.stroke();
        }
      });
    });
  });

  ctx2d.shadowBlur = 0;
  ctx2d.font = '12px Space Grotesk, sans-serif';
  ctx2d.textAlign = 'center';
  ctx2d.textBaseline = 'bottom';
  ctx2d.fillStyle = 'rgba(255, 236, 178, 0.92)';

  ranges.forEach((range) => {
    const point = sunPathPointAtMinute(segments, range.from + GOLDEN_HOUR_MINUTES / 2);
    if (point) {
      ctx2d.fillText(sunText('bands.goldenHour'), point.x, point.y - 10);
    }
  });

  ctx2d.restore();
}

function draw() {
  const w = canvas.width;
  const h = canvas.height;
  const pad = { left: 74, right: 28, top: 31, bottom: 62 };
  const cw = w - pad.left - pad.right;
  const ch = h - pad.top - pad.bottom;

  ctx.clearRect(0, 0, w, h);

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

  fillBand(-90, -18, 'rgba(4, 6, 20, 0.60)');
  fillBand(-18, -12, 'rgba(18, 28, 72, 0.45)');
  fillBand(-12, -6, 'rgba(38, 68, 148, 0.38)');
  fillBand(-6, 0, 'rgba(255, 174, 74, 0.24)');
  fillBand(0, 90, 'rgba(255, 196, 96, 0.16)');

  const bands = [
    { from: -90, to: -18, label: sunText('bands.night'), color: 'rgba(140, 160, 220, 0.55)' },
    { from: -18, to: -12, label: sunText('bands.astronomical'), color: 'rgba(140, 170, 230, 0.55)' },
    { from: -12, to: -6, label: sunText('bands.nautical'), color: 'rgba(160, 190, 240, 0.60)' },
    { from: -6, to: 0, label: sunText('bands.sunset'), color: 'rgba(255, 210, 140, 0.72)' }
  ];

  ctx.font = '11px Space Grotesk, sans-serif';
  ctx.textBaseline = 'top';
  bands.forEach(({ from, to, label, color }) => {
    const yTop = Math.min(yByElev(from), yByElev(to));
    const bandH = Math.abs(yByElev(to) - yByElev(from));
    if (bandH < 10) {
      return;
    }
    ctx.fillStyle = color;
    ctx.fillText(label, pad.left + 6, yTop + 3);
  });
  ctx.textBaseline = 'alphabetic';

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
    ctx.fillText(String(elev) + '\u00B0', 30, y + 4);
  }

  [
    { elev: 0, label: '0\u00B0', color: 'rgba(255, 210, 140, 0.86)' },
    { elev: -6, label: '-6\u00B0', color: 'rgba(190, 215, 255, 0.78)' },
    { elev: -12, label: '-12\u00B0', color: 'rgba(170, 195, 245, 0.78)' },
    { elev: -18, label: '-18\u00B0', color: 'rgba(160, 185, 240, 0.78)' }
  ].forEach(({ elev, label, color }) => {
    const y = yByElev(elev);
    ctx.save();
    ctx.strokeStyle = 'rgba(140, 160, 220, 0.40)';
    ctx.lineWidth = 1;
    ctx.setLineDash([3, 4]);
    ctx.beginPath();
    ctx.moveTo(pad.left - 4, y);
    ctx.lineTo(pad.left, y);
    ctx.moveTo(pad.left, y);
    ctx.lineTo(w - pad.right, y);
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.fillStyle = color;
    ctx.font = '11px Space Grotesk, sans-serif';
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    ctx.fillText(label, pad.left - 8, y);
    ctx.restore();
  });

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

  const sunPathSegments = buildSunPathSegments(currentData.path, xByAz, yByElev);
  drawSunPathSegments(ctx, sunPathSegments, pad.left, pad.top, cw, ch);
  drawGoldenHourRanges(ctx, sunPathSegments, currentData.events || {});

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

  const currentInfo = [
    [sunText('stats.currentTime'), minuteToText(now.minuteOfDay)],
    [sunText('stats.mode'), mode],
    [sunText('stats.azimuth'), deg(now.azimuth)],
    [sunText('stats.elevation'), deg(now.elevation)],
    [sunText('stats.zenith'), deg(now.zenith)]
  ];

  const dayBoundsInfo = [];
  if (Array.isArray(data.path) && data.path.length > 0) {
    const stats = {
      min: Math.min(...data.path.map(p => p.elevation)).toFixed(1),
      max: Math.max(...data.path.map(p => p.elevation)).toFixed(1)
    };
    dayBoundsInfo.push([sunText('stats.minElevation'), stats.min + '\u00B0']);
    dayBoundsInfo.push([sunText('stats.maxElevation'), stats.max + '\u00B0']);
  }

  const groups = [
    statGroupHtml(sunText('groups.current'), currentInfo)
  ];
  if (dayBoundsInfo.length > 0) {
    groups.push(statGroupHtml(sunText('groups.dayBounds'), dayBoundsInfo));
  }
  statsEl.innerHTML = groups.join('');
}

function renderEvents(data) {
  const ev = data.events || {};

  function durText(minutes) {
    const h = Math.floor(minutes / 60);
    const m = minutes % 60;
    return h + sunText('units.hour') + ' ' + String(m).padStart(2, '0') + sunText('units.minute');
  }

  const path = Array.isArray(data.path) ? data.path : [];
  const elevations = path.map(p => Number(p.elevation)).filter(Number.isFinite);
  const minElevation = elevations.length > 0 ? Math.min(...elevations) : null;
  const reachesBelow = (boundary) => Number.isFinite(minElevation) && minElevation < boundary;

  function crossingsFor(threshold) {
    const crossings = [];
    path.forEach((point) => {
      const minute = Number(point.minute);
      const elevation = Number(point.elevation);
      if (Number.isFinite(minute) && Number.isFinite(elevation) && Math.abs(elevation - threshold) < 0.01) {
        if (!crossings.some(existing => Math.abs(existing - minute) < 0.02)) {
          crossings.push(minute);
        }
      }
    });
    return crossings.sort((a, b) => a - b);
  }

  function minuteRange(from, to) {
    return minuteToText(from) + ' - ' + minuteToText(to);
  }

  function boundaryRange(boundary) {
    const crossings = crossingsFor(boundary);
    if (crossings.length < 2 || !reachesBelow(boundary)) {
      return emptyTimeRange();
    }
    return minuteRange(crossings[crossings.length - 1], crossings[0]);
  }

  function twilightBandRange(upperBoundary, lowerBoundary) {
    const upper = crossingsFor(upperBoundary);
    const lower = crossingsFor(lowerBoundary);
    if (upper.length < 2 || !reachesBelow(upperBoundary)) {
      return emptyTimeRange();
    }

    const morningUpper = upper[0];
    const eveningUpper = upper[upper.length - 1];

    if (lower.length >= 2 && reachesBelow(lowerBoundary)) {
      const morningLower = lower[0];
      const eveningLower = lower[lower.length - 1];
      return minuteRange(eveningUpper, eveningLower) + ' / ' + minuteRange(morningLower, morningUpper);
    }

    return minuteRange(eveningUpper, morningUpper);
  }

  function nightRange() {
    const astronomical = crossingsFor(-18);
    if (astronomical.length < 2 || !reachesBelow(-18)) {
      return emptyTimeRange();
    }
    return minuteRange(astronomical[astronomical.length - 1], astronomical[0]);
  }

  const dayDur = (ev.hasSunrise && ev.hasSunset) ? durText(ev.sunset - ev.sunrise) : null;
  const nightDur = (ev.hasSunrise && ev.hasSunset)
    ? durText(ev.sunrise + (DAY_MINUTES - ev.sunset))
    : null;

  const durationItems = [
    [sunText('events.dayLength'), dayDur || (ev.isPolarDay ? sunText('events.polarDay') : sunText('events.none'))],
    [sunText('events.nightLength'), nightDur || (ev.isPolarNight ? sunText('events.polarNight') : sunText('events.none'))]
  ];

  const sunEventItems = [
    [sunText('events.sunrise'), ev.hasSunrise ? minuteToText(ev.sunrise) : '--:--'],
    [sunText('events.sunset'), ev.hasSunset ? minuteToText(ev.sunset) : '--:--']
  ];

  const goldenHourItems = [
    [sunText('events.goldenBeforeSunset'), ev.hasSunset ? minuteRange(ev.sunset - GOLDEN_HOUR_MINUTES, ev.sunset) : emptyTimeRange()],
    [sunText('events.goldenAfterSunrise'), ev.hasSunrise ? minuteRange(ev.sunrise, ev.sunrise + GOLDEN_HOUR_MINUTES) : emptyTimeRange()]
  ];

  const twilightItems = [
    [sunText('events.civil'), boundaryRange(0)],
    [sunText('events.nautical'), twilightBandRange(-6, -12)],
    [sunText('events.astronomical'), twilightBandRange(-12, -18)],
    [sunText('events.night'), nightRange()]
  ];

  if (ev.isPolarDay) {
    sunEventItems.push([sunText('events.polarStatus'), sunText('events.polarDay')]);
  }
  if (ev.isPolarNight) {
    sunEventItems.push([sunText('events.polarStatus'), sunText('events.polarNight')]);
  }

  eventsEl.innerHTML = [
    statGroupHtml(sunText('groups.durations'), durationItems),
    statGroupHtml(sunText('groups.sunEvents'), sunEventItems),
    statGroupHtml(sunText('groups.goldenHour'), goldenHourItems),
    statGroupHtml(sunText('groups.twilight'), twilightItems)
  ].join('');
}

function updateMeta(data) {
  const d = data.date || {};
  const offset = data.timezoneOffsetMinutes;
  const location = data.location || {};
  const latText = Number.isFinite(location.lat) ? location.lat.toFixed(6) : '--';
  const lonText = Number.isFinite(location.lon) ? location.lon.toFixed(6) : '--';
  metaEl.textContent =
    sunText('meta.date') + ': ' + String(d.year || '--') + '-' + String(d.month || '--').padStart(2, '0') + '-' + String(d.day || '--').padStart(2, '0') +
    ' | ' + sunText('meta.utcOffset') + ': ' + (Number.isFinite(offset) ? (offset >= 0 ? '+' : '') + offset + ' min' : '--') +
    ' | ' + sunText('meta.latLon') + ': ' + latText + ', ' + lonText;

  warnEl.textContent = data.validTime ? '' : sunText('meta.ntpWarning');
}

function locationFromApiSettings(json) {
  const lat = normalizeCoord(json?.lat ?? json?.location?.lat);
  const lon = normalizeCoord(json?.lon ?? json?.location?.lng);
  return lat === null || lon === null ? null : { lat, lon };
}

function locationFromStaticSettings(json) {
  const lat = normalizeCoord(json?.location?.lat);
  const lon = normalizeCoord(json?.location?.lng);
  return lat === null || lon === null ? null : { lat, lon };
}

async function readSettingsLocation(url, mapLocation) {
  const res = await fetch(url, { cache: 'no-store' });
  const json = await readJsonResponse(res);
  if (!res.ok) {
    throw new Error(json.message || sunText('errors.settingsJson'));
  }

  const location = mapLocation(json);
  if (!location) {
    throw new Error(sunText('errors.locationMissing'));
  }
  return location;
}

async function loadLocationFromSettings() {
  try {
    locationFromSettings = await readSettingsLocation('/api/settings', locationFromApiSettings);
  } catch (apiErr) {
    try {
      locationFromSettings = await readSettingsLocation('/settings.json', locationFromStaticSettings);
    } catch (staticErr) {
      console.error(apiErr);
      console.error(staticErr);
      locationFromSettings = null;
      warnEl.textContent = sunText('errors.settingsCoordinates');
    }
  }
}

async function refresh() {
  const params = new URLSearchParams();

  if (locationFromSettings) {
    params.set('lat', String(locationFromSettings.lat));
    params.set('lon', String(locationFromSettings.lon));
  }

  const url = '/api/sun' + (params.toString() ? '?' + params.toString() : '');

  metaEl.textContent = sunText('meta.updating');
  try {
    const res = await fetch(url, { cache: 'no-store' });
    const json = await readJsonResponse(res);

    if (!res.ok || json.status !== 'ok') {
      throw new Error(json.message || sunText('errors.sunApi'));
    }

    currentData = json;
    updateMeta(json);
    renderStats(json);
    renderEvents(json);
    draw();
  } catch (err) {
    console.error(err);
    metaEl.textContent = sunText('errors.dataLoad');
    warnEl.textContent = err.message || sunText('errors.sunFetch');
  }
}

window.addEventListener('resize', draw);

(async () => {
  applyStaticLocale();
  await loadLocationFromSettings();
  await refresh();
  window.setInterval(refresh, 60000);
})();
