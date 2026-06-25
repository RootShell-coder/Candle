(function () {
  const Candle = window.Candle;

  function applySettings(settings) {
    window.CandleLive?.applySettings(settings);
    window.CandleSettings?.applySettings(settings);
  }

  function bindModules() {
    window.CandleLive?.bind();
    window.CandleSettings?.bind();
    window.CandleUpdate?.bind();
  }

  bindModules();
  Candle.loadSettings()
    .then(applySettings)
    .catch((err) => {
      Candle.showToast(err.message || Candle.t('app.settingsUnavailable'), 'error', 0);
    });
}());
