window.CandleSunLocale = {
  pageTitle: 'Солнце - Candle Light',
  heading: 'Солнце',
  subtitle: 'Положение солнца и режимы автоматического управления',
  navLabel: 'Основное меню',
  nav: {
    home: 'Главная',
    settings: 'Настройки',
    update: 'Обновление',
    matrix: 'Матрица',
    sun: 'Солнце',
    moon: 'Луна'
  },
  panelLabel: 'Солнце',
  panelTitle: 'Солнечный путь',
  loading: 'Загрузка...',
  canvasLabel: 'График пути солнца',
  errors: {
    emptyResponse: 'Сервер вернул пустой ответ',
    invalidJson: 'Сервер вернул некорректный JSON: ',
    settingsJson: 'Ошибка settings.json',
    locationMissing: 'В settings.json не найдены location.lat/lng',
    settingsCoordinates: 'Не удалось прочитать координаты из settings.json; используются координаты устройства.',
    sunApi: 'Ошибка API солнца',
    dataLoad: 'Ошибка загрузки данных',
    sunFetch: 'Не удалось получить данные о солнце'
  },
  meta: {
    date: 'Дата',
    utcOffset: 'Смещение UTC',
    latLon: 'широта/долгота',
    updating: 'Обновление...',
    ntpWarning: 'Внимание: NTP-время не подтверждено; расчеты могут быть неточными.'
  },
  bands: {
    night: 'Ночь',
    astronomical: 'Астрон. сумерки',
    nautical: 'Навиг. сумерки',
    civil: 'Гражд. сумерки',
    sunset: 'Закат',
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
    sunEvents: 'События',
    goldenHour: 'Золотой час',
    twilight: 'Сумерки'
  },
  units: {
    hour: 'ч',
    minute: 'м'
  }
};
