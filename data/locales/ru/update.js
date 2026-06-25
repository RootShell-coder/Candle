window.CandleLocale = {
  ...(window.CandleLocale || {}),
  update: {
    noFile: 'Выберите {label} .bin файл',
    wrongFile: '{label} должен быть .bin файлом',
    fileMissing: 'Файл не выбран',
    uploadingFirmware: 'Загрузка прошивки: {name}',
    uploadingLittlefs: 'Загрузка LittleFS: {name}',
    uploadedRestart: 'Файлы загружены. Перезагрузка устройства...',
    restartFailed: 'Не удалось отправить команду перезагрузки',
    uploadedWaiting: 'Обновление загружено. Жду запуск устройства...',
    failed: 'Обновление не выполнено',
    networkError: 'Сетевая ошибка при загрузке {type}',
    firmwareLabel: 'прошивку',
    littlefsLabel: 'LittleFS',
    firmwareType: 'прошивки',
    filesystemType: 'LittleFS',
    updateFailed: 'Обновление {type} не выполнено'
  }
};
