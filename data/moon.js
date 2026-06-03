'use strict';

const SYNODIC = 29.53058770576;

const PHASE_NAMES = [
  'Новолуние',
  'Молодой месяц',
  'Первая четверть',
  'Прибывающая луна',
  'Полнолуние',
  'Убывающая луна',
  'Последняя четверть',
  'Стареющий месяц',
];

// ─── Moon drawing ─────────────────────────────────────────────────────────────

/**
 * Draws a moon disc at (cx, cy) with radius r.
 * phase: 0..1  (0 = new moon, 0.5 = full moon)
 *
 * Algorithm:
 *  - Fill dark base circle.
 *  - For the lit region draw a closed path consisting of:
 *      waxing (phase 0..0.5): right outer arc + terminator ellipse back.
 *      waning (phase 0.5..1): left outer arc  + terminator ellipse back.
 *  - Terminator x-radius = r * cos(2π * phase)  [waxing]
 *                         = r * cos(2π * (phase - 0.5)) [waning]
 *    Positive → ellipse curves toward lit side (crescent).
 *    Negative → ellipse curves away  (gibbous).
 */
function drawMoonDisc(ctx, cx, cy, r, phase) {
  const DARK  = '#0d0e13';
  const LIGHT = '#d4c4a8';
  const CRATERS = [
    { dx: 0.22, dy: -0.18, r: 0.09 },
    { dx: -0.28, dy: 0.14, r: 0.07 },
    { dx: 0.05, dy: 0.30, r: 0.11 },
    { dx: -0.12, dy: -0.32, r: 0.06 },
    { dx: 0.30, dy: 0.08, r: 0.05 },
  ];

  const PI  = Math.PI;
  const PI2 = 2 * PI;

  // Outer glow (visible only when not new moon)
  if (phase > 0.03 && phase < 0.97) {
    const glowR = r * 1.35;
    const glow  = ctx.createRadialGradient(cx, cy, r * 0.82, cx, cy, glowR);
    const alpha = Math.sin(PI * phase) * 0.14;
    glow.addColorStop(0, 'rgba(220, 200, 160, ' + alpha + ')');
    glow.addColorStop(1, 'rgba(0,0,0,0)');
    ctx.beginPath();
    ctx.arc(cx, cy, glowR, 0, PI2);
    ctx.fillStyle = glow;
    ctx.fill();
  }

  // Dark base
  ctx.beginPath();
  ctx.arc(cx, cy, r, 0, PI2);
  ctx.fillStyle = DARK;
  ctx.fill();

  const isNew  = phase < 0.015 || phase > 0.985;
  const isFull = Math.abs(phase - 0.5) < 0.015;

  if (!isNew) {
    ctx.save();
    // Clip all lit drawing to the disc
    ctx.beginPath();
    ctx.arc(cx, cy, r, 0, PI2);
    ctx.clip();

    ctx.beginPath();

    if (isFull) {
      ctx.arc(cx, cy, r, 0, PI2);
    } else if (phase < 0.5) {
      // Waxing: lit on right
      const rx = r * Math.cos(PI2 * phase); // +r → crescent, -r → gibbous
      ctx.arc(cx, cy, r, -PI / 2, PI / 2, false); // CW: top→right→bottom
      if (rx >= 0) {
        // Terminator curves right (crescent): CCW bottom→right→top
        ctx.ellipse(cx, cy, rx, r, 0, PI / 2, -PI / 2, true);
      } else {
        // Terminator curves left (gibbous): CW bottom→left→top
        ctx.ellipse(cx, cy, -rx, r, 0, PI / 2, -PI / 2, false);
      }
    } else {
      // Waning: lit on left
      const sub = phase - 0.5;
      const rx  = r * Math.cos(PI2 * sub); // +r → gibbous, -r → crescent
      ctx.arc(cx, cy, r, -PI / 2, PI / 2, true); // CCW: top→left→bottom
      if (rx >= 0) {
        // Terminator curves right (gibbous): CCW bottom→right→top
        ctx.ellipse(cx, cy, rx, r, 0, PI / 2, -PI / 2, true);
      } else {
        // Terminator curves left (crescent): CW bottom→left→top
        ctx.ellipse(cx, cy, -rx, r, 0, PI / 2, -PI / 2, false);
      }
    }

    ctx.closePath();
    ctx.fillStyle = LIGHT;
    ctx.fill();

    // Subtle surface texture (craters) visible on lit side
    if (r > 30) {
      ctx.globalAlpha = 0.10;
      for (const c of CRATERS) {
        ctx.beginPath();
        ctx.arc(cx + c.dx * r, cy + c.dy * r, c.r * r, 0, PI2);
        ctx.fillStyle = 'rgba(0,0,0,0.6)';
        ctx.fill();
      }
      ctx.globalAlpha = 1;
    }

    ctx.restore();
  }

  // Limb
  ctx.beginPath();
  ctx.arc(cx, cy, r, 0, PI2);
  ctx.strokeStyle = 'rgba(180, 160, 130, 0.20)';
  ctx.lineWidth   = Math.max(1, r * 0.012);
  ctx.stroke();
}

function drawStars(ctx, w, h) {
  // Deterministic pseudo-random stars based on a fixed seed table
  const pts = [
    [0.08, 0.12], [0.18, 0.55], [0.31, 0.08], [0.44, 0.78],
    [0.57, 0.22], [0.63, 0.65], [0.72, 0.40], [0.82, 0.15],
    [0.90, 0.72], [0.95, 0.35], [0.14, 0.88], [0.38, 0.47],
    [0.50, 0.93], [0.76, 0.85], [0.25, 0.30],
  ];
  const sizes = [0.8, 0.5, 1.0, 0.6, 0.9, 0.4, 0.7, 1.1, 0.5, 0.8, 0.6, 0.9, 0.4, 0.7, 1.0];
  const alphas= [0.35, 0.50, 0.28, 0.60, 0.42, 0.55, 0.38, 0.65, 0.30, 0.48, 0.58, 0.33, 0.62, 0.45, 0.52];

  for (let i = 0; i < pts.length; i++) {
    ctx.beginPath();
    ctx.arc(pts[i][0] * w, pts[i][1] * h, sizes[i], 0, Math.PI * 2);
    ctx.fillStyle = 'rgba(255,255,255,' + alphas[i] + ')';
    ctx.fill();
  }
}

// ─── Cycle strip ──────────────────────────────────────────────────────────────

function buildCycleStrip(container, activePhaseIndex) {
  container.innerHTML = '';

  for (let i = 0; i < 8; i++) {
    const p = i / 8; // canonical normalised phase for icon (0, 0.125, …, 0.875)

    const item = document.createElement('div');
    item.className = 'cycle-item' + (i === activePhaseIndex ? ' active' : '');

    const cvs = document.createElement('canvas');
    cvs.width  = 56;
    cvs.height = 56;
    cvs.className = 'cycle-canvas';
    const cc = cvs.getContext('2d');
    // Dark bg
    cc.fillStyle = '#0d0e13';
    cc.fillRect(0, 0, 56, 56);
    drawMoonDisc(cc, 28, 28, 22, p);

    const lbl = document.createElement('span');
    lbl.className   = 'cycle-label';
    lbl.textContent = PHASE_NAMES[i];

    item.appendChild(cvs);
    item.appendChild(lbl);
    container.appendChild(item);
  }
}

// ─── Main render ──────────────────────────────────────────────────────────────

function render(data) {
  const canvas  = document.getElementById('moonCanvas');
  const infoEl  = document.getElementById('moonInfo');
  const cycleEl = document.getElementById('moonCycle');
  const ctx     = canvas.getContext('2d');
  const W = canvas.width;
  const H = canvas.height;

  const ageDays     = data.ageDays;
  const illumination= data.illumination;
  const phaseIndex  = data.phase;          // integer 0..7
  const phaseName   = PHASE_NAMES[phaseIndex] || data.phaseName;
  const phase       = ageDays / SYNODIC;   // normalised 0..1

  // Background
  ctx.fillStyle = '#0a0b0f';
  ctx.fillRect(0, 0, W, H);
  drawStars(ctx, W, H);

  // Moon disc
  const moonR = Math.round(Math.min(W, H) * 0.36);
  drawMoonDisc(ctx, W / 2, H / 2, moonR, phase);

  // Phase name inside canvas (bottom)
  ctx.font      = 'bold ' + Math.round(W * 0.048) + 'px "Segoe UI", Arial, sans-serif';
  ctx.textAlign = 'center';
  ctx.fillStyle = 'rgba(230, 210, 180, 0.82)';
  ctx.fillText(phaseName, W / 2, H - Math.round(H * 0.07));

  // Info cards
  const daysLeft = SYNODIC - ageDays;
  const pct      = Math.round(illumination * 100);

  infoEl.innerHTML =
    '<div class="moon-stat"><span class="moon-stat-k">Название фазы</span>' +
      '<span class="moon-stat-v">' + phaseName + '</span></div>' +
    '<div class="moon-stat"><span class="moon-stat-k">Возраст луны</span>' +
      '<span class="moon-stat-v">' + ageDays.toFixed(2) + ' сут</span></div>' +
    '<div class="moon-stat"><span class="moon-stat-k">Освещённость диска</span>' +
      '<span class="moon-stat-v">' + pct + '%</span></div>' +
    '<div class="moon-stat"><span class="moon-stat-k">До новолуния</span>' +
      '<span class="moon-stat-v">' + daysLeft.toFixed(1) + ' сут</span></div>';

  // Cycle
  buildCycleStrip(cycleEl, phaseIndex);
}

async function init() {
  let data = null;
  try {
    const resp = await fetch('/api/moon', { cache: 'no-store' });
    if (!resp.ok) { throw new Error('HTTP ' + resp.status); }
    const raw = await resp.text();
    data = JSON.parse(raw);
  } catch (e) {
    document.getElementById('moonInfo').textContent =
      'Нет данных — время не синхронизировано';
    return;
  }

  if (!data || data.status !== 'ok') {
    document.getElementById('moonInfo').textContent =
      'Ошибка: ' + (data && data.message ? data.message : 'неизвестная ошибка');
    return;
  }

  render(data);
}

init();
