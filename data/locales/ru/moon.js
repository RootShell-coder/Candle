window.CandleMoonLocale = {
  phases: [
    'Новолуние',
    'Молодой месяц',
    'Первая четверть',
    'Прибывающая луна',
    'Полнолуние',
    'Убывающая луна',
    'Последняя четверть',
    'Стареющий месяц'
  ],
  stats: {
    phase: 'Фаза',
    age: 'Возраст луны',
    illumination: 'Освещенность диска',
    nextNewMoon: 'До новолуния'
  },
  led: {
    title: 'Лунная подсветка',
    enableTitle: 'Включить WLED',
    enableHint: 'Светится только когда включена основная анимация.',
    maxBrightness: 'Верхняя яркость',
    hue: 'Оттенок свечения',
    hueValue: '{hue}°',
    colorValue: 'hsl({hue} 100% 50%)',
    initialDetails: 'Текущая расчетная яркость: --',
    enabled: 'Включено',
    disabled: 'Выключено',
    pinMissing: 'Пин не задан',
    calculatedBrightness: 'Текущая расчетная яркость: {calculated}% / выход: {current}%',
    saveError: 'Не удалось сохранить настройки лунной подсветки',
    genericSaveError: 'Ошибка сохранения'
  },
  errors: {
    emptyResponse: 'Пустой ответ сервера',
    invalidJson: 'Сервер вернул некорректный JSON: {preview}',
    noMoonData: 'Нет данных о луне'
  },
  units: {
    days: 'сут'
  }
};
