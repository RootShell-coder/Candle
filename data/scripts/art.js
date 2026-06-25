(() => {
  const MATRIX_WIDTH = 9;
  const MATRIX_HEIGHT = 16;
  const FRAME_DELAY_MS = 40;
  const METRICS_REFRESH_INTERVAL_MS = 30000;
  const ANIM_DATA = window.CANDLE_ART_DATA || [];

  const EST_FULL_MATRIX_CURRENT_MA = 500;
  const matrixEl = document.getElementById('artMatrix') || document.getElementById('matrix');
  const statusEl =
    document.getElementById('artStatus') ||
    (matrixEl && matrixEl.id === 'matrix' ? document.getElementById('status') : null);
  const techFrameValueEl = document.getElementById('techFrameValue');
  const techAreaValueEl = document.getElementById('techAreaValue');
  const techAreaSizeEl = document.getElementById('techAreaSize');
  const techSpeedValueEl = document.getElementById('techSpeedValue');
  const techCurrentValueEl = document.getElementById('techCurrentValue');
  const techMetricsInfoEl = document.getElementById('techMetricsInfo');
  const techExplanationEl = document.getElementById('techExplanation');
  const manualModeEl = document.getElementById('manualMode');
  const autoModeEl = document.getElementById('autoMode');
  const autoTimeModeEl = document.getElementById('autoTimeMode');
  const brightnessEl = document.getElementById('brightness');

  if (!matrixEl) {
    return;
  }
  const cells = [];
  const pixels = new Uint8Array(MATRIX_WIDTH * MATRIX_HEIGHT);

  const indexFor = (x, y) => (x * MATRIX_HEIGHT) + y;

  function isAnimationEnabled() {
    if (manualModeEl || autoModeEl || autoTimeModeEl) {
      if (manualModeEl?.checked) {
        return true;
      }
      if (autoModeEl?.checked) {
        return !!window.Candle?.state?.liveAutoCandleOn;
      }
      if (autoTimeModeEl?.checked) {
        return !!window.Candle?.state?.liveAutoTimeCandleOn;
      }
      return false;
    }

    if (!brightnessEl) {
      return true;
    }

    const brightness = brightnessEl
      ? Number.parseInt(brightnessEl.value || '0', 10)
      : 100;

    if (Number.isFinite(brightness) && brightness <= 0) {
      return false;
    }

    return true;
  }

  function createMatrix() {
    for (let y = 0; y < MATRIX_HEIGHT; y++) {
      for (let x = 0; x < MATRIX_WIDTH; x++) {
        const cell = document.createElement('div');
        cell.className = 'pixel';
        cell.style.gridColumn = String(x + 1);
        cell.style.gridRow = String(y + 1);
        cell.title = `x:${x}, y:${y}`;
        matrixEl.appendChild(cell);
        cells[indexFor(x, y)] = cell;
      }
    }
  }

  function countFrames() {
    let cursor = 0;
    let total = 0;

    while (cursor < ANIM_DATA.length) {
      const start = ANIM_DATA[cursor++];
      if (start >= 0x90) {
        break;
      }

      const end = ANIM_DATA[cursor++];
      const x1 = start >> 4;
      const y1 = start & 0x0F;
      const x2 = end >> 4;
      const y2 = end & 0x0F;
      cursor += (x2 - x1 + 1) * (y2 - y1 + 1);
      total++;
    }

    return total;
  }

  function paintPixel(cell, value) {
    const ratio = value / 255;

    if (ratio <= 0) {
      cell.style.background = 'rgba(255, 255, 255, 0.06)';
      cell.style.boxShadow = 'inset 0 0 0 1px rgba(255, 255, 255, 0.04)';
      return;
    }

    const lightness = 18 + Math.round(ratio * 54);
    const alpha = 0.25 + ratio * 0.75;
    const glow = (4 + ratio * 14).toFixed(1);

    cell.style.background = `hsla(34, 100%, ${lightness}%, ${alpha})`;
    cell.style.boxShadow = `0 0 ${glow}px hsla(38, 100%, 68%, ${0.25 + ratio * 0.55}), inset 0 0 0 1px rgba(255, 248, 220, 0.18)`;
  }

  function renderFrame() {
    for (let y = 0; y < MATRIX_HEIGHT; y++) {
      for (let x = 0; x < MATRIX_WIDTH; x++) {
        paintPixel(cells[indexFor(x, y)], pixels[indexFor(x, y)]);
      }
    }
  }

  function applyNextFrame(cursor) {
    let nextCursor = cursor;
    let start = ANIM_DATA[nextCursor++];

    if (start >= 0x90) {
      pixels.fill(0);
      nextCursor = 0;
      start = ANIM_DATA[nextCursor++];
    }

    const end = ANIM_DATA[nextCursor++];
    const x1 = start >> 4;
    const y1 = start & 0x0F;
    const x2 = end >> 4;
    const y2 = end & 0x0F;

    for (let x = x1; x <= x2; x++) {
      for (let y = y1; y <= y2; y++) {
        pixels[indexFor(x, y)] = ANIM_DATA[nextCursor++];
      }
    }

    return { nextCursor, area: [x1, y1, x2, y2] };
  }

  function estimateCurrentMa(pixelBuffer) {
    let totalBrightness = 0;
    for (const value of pixelBuffer) {
      totalBrightness += value;
    }

    return Math.round(
      (totalBrightness / (pixelBuffer.length * 255)) * EST_FULL_MATRIX_CURRENT_MA
    );
  }

  function estimateAverageCurrentMa() {
    const simulationPixels = new Uint8Array(MATRIX_WIDTH * MATRIX_HEIGHT);
    let cursor = 0;
    let totalCurrentMa = 0;
    let frames = 0;

    while (cursor < ANIM_DATA.length) {
      const start = ANIM_DATA[cursor++];
      if (start >= 0x90) {
        break;
      }

      const end = ANIM_DATA[cursor++];
      const x1 = start >> 4;
      const y1 = start & 0x0F;
      const x2 = end >> 4;
      const y2 = end & 0x0F;

      for (let x = x1; x <= x2; x++) {
        for (let y = y1; y <= y2; y++) {
          simulationPixels[indexFor(x, y)] = ANIM_DATA[cursor++];
        }
      }

      totalCurrentMa += estimateCurrentMa(simulationPixels);
      frames += 1;
    }

    return frames > 0 ? Math.round(totalCurrentMa / frames) : 0;
  }

  function updateMetricsInfo() {
    if (!techMetricsInfoEl) {
      return;
    }

    fetch('/metrics', { cache: 'no-store' })
      .then((response) => {
        if (!response.ok) {
          throw new Error('Не удалось загрузить метрики');
        }
        return response.text();
      })
      .then((text) => {
        const uptimeMatch = text.match(/^candle_uptime_seconds\s+([0-9.]+)/m);
        const unixtimeMatch = text.match(/^candle_current_unixtime\s+([0-9.]+)/m);

        const uptimeSeconds = uptimeMatch ? Number.parseFloat(uptimeMatch[1]) : NaN;
        const currentUnixtime = unixtimeMatch ? Number.parseInt(unixtimeMatch[1], 10) : 0;

        const uptimeText = Number.isFinite(uptimeSeconds)
          ? Math.floor(uptimeSeconds).toString()
          : '--';
        const unixText = currentUnixtime > 0
          ? currentUnixtime.toString()
          : '--';

        techMetricsInfoEl.textContent = `Аптайм: ${uptimeText} с • Unix: ${unixText}`;
      })
      .catch(() => {
        techMetricsInfoEl.textContent = 'Аптайм: -- с • Unix: --';
      });
  }

  function updateTechnicalInfo(frameValue, totalFrameCount, area) {
    if (!Array.isArray(area) || area.length !== 4) {
      return;
    }

    const [x1, y1, x2, y2] = area;
    const areaWidth = Math.max(0, x2 - x1 + 1);
    const areaHeight = Math.max(0, y2 - y1 + 1);
    const fps = (1000 / FRAME_DELAY_MS).toFixed(1);

    let activePixels = 0;
    let totalBrightness = 0;
    for (const value of pixels) {
      totalBrightness += value;
      if (value > 0) {
        activePixels += 1;
      }
    }

    const avgActiveBrightnessPct = activePixels > 0
      ? Math.round((totalBrightness / (activePixels * 255)) * 100)
      : 0;
    const currentCurrentMa = estimateCurrentMa(pixels);

    if (techFrameValueEl) {
      techFrameValueEl.textContent = `${frameValue} / ${totalFrameCount}`;
    }
    if (techAreaValueEl) {
      techAreaValueEl.textContent = `${x1}, ${y1}, ${x2}, ${y2}`;
    }
    if (techAreaSizeEl) {
      techAreaSizeEl.textContent = `${areaWidth} × ${areaHeight} px`;
    }
    if (techSpeedValueEl) {
      techSpeedValueEl.textContent = `${FRAME_DELAY_MS} мс ≈ ${fps} FPS`;
    }
    if (techCurrentValueEl) {
      techCurrentValueEl.textContent = `~${currentCurrentMa} / ~${averageCurrentMa} мА`;
    }
    if (techExplanationEl) {
      techExplanationEl.textContent =
        `Потребление: текущий кадр ~${currentCurrentMa} мА, среднее по ${totalFrameCount} кадрам ~${averageCurrentMa} мА. Активно ${activePixels}/${pixels.length} LED, средняя яркость ${avgActiveBrightnessPct}%.`;
    }
  }

  createMatrix();

  const totalFrames = countFrames();
  const averageCurrentMa = estimateAverageCurrentMa();
  let cursor = 0;
  let frame = 0;

  const tick = () => {
    if (!isAnimationEnabled()) {
      pixels.fill(0);
      renderFrame();
      if (statusEl) {
        statusEl.textContent = 'Анимация остановлена';
      }
      return;
    }

    const result = applyNextFrame(cursor);
    cursor = result.nextCursor;
    frame = (frame % totalFrames) + 1;
    renderFrame();

    const [x1, y1, x2, y2] = result.area;
    const areaWidth = Math.max(0, x2 - x1 + 1);
    const areaHeight = Math.max(0, y2 - y1 + 1);

    if (statusEl) {
      statusEl.textContent =
        `Кадр ${frame} из ${totalFrames} • X=${x1}…${x2}, Y=${y1}…${y2} • ${areaWidth}×${areaHeight} px`;
    }

    updateTechnicalInfo(frame, totalFrames, result.area);
  };

  tick();
  updateMetricsInfo();
  window.setInterval(tick, FRAME_DELAY_MS);
  window.setInterval(updateMetricsInfo, METRICS_REFRESH_INTERVAL_MS);
})();
