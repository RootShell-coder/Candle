(function () {
  const Candle = window.Candle;
  const { els, t } = Candle;

  function setUpdateControlsDisabled(disabled) {
    [els.firmwareFile, els.filesystemFile, els.updateBtn].filter(Boolean).forEach((element) => {
      element.disabled = disabled;
    });
  }

  function setUpdateStatus(message, className = '') {
    if (!els.updateStatus) {
      return;
    }
    els.updateStatus.textContent = message;
    els.updateStatus.className = `update-status ${className}`.trim();
  }

  function setUpdateProgress(value, visible = true) {
    if (!els.updateProgress) {
      return;
    }
    els.updateProgress.style.display = visible ? 'block' : 'none';
    els.updateProgress.value = Math.max(0, Math.min(100, value));
  }

  function updateFileMeta(input, metaEl) {
    if (!input || !metaEl) {
      return;
    }
    const file = input.files && input.files[0];
    metaEl.textContent = file ? `${file.name} / ${Candle.formatBytes(file.size)}` : t('update.fileMissing');
  }

  function selectedUpdateFile(fileInput, label) {
    if (!fileInput || !fileInput.files || fileInput.files.length === 0) {
      throw new Error(t('update.noFile', { label }));
    }
    const file = fileInput.files[0];
    if (!file.name.toLowerCase().endsWith('.bin')) {
      throw new Error(t('update.wrongFile', { label }));
    }
    return file;
  }

  function uploadBinaryUpdate(type, file, progressStart, progressEnd, label = type) {
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      const endpoint = type === 'filesystem' ? '/api/update/filesystem' : '/api/update/firmware';
      xhr.open('POST', endpoint);
      xhr.responseType = 'json';
      xhr.setRequestHeader('Content-Type', 'application/octet-stream');
      xhr.setRequestHeader('X-Update-Filename', file.name);

      xhr.upload.onprogress = (event) => {
        if (!event.lengthComputable) {
          setUpdateProgress(progressStart);
          return;
        }
        const localProgress = event.loaded / event.total;
        setUpdateProgress(Math.round(progressStart + (progressEnd - progressStart) * localProgress));
      };

      xhr.onload = () => {
        const response = xhr.response || {};
        if (xhr.status >= 200 && xhr.status < 300 && response.status === 'ok') {
          setUpdateProgress(progressEnd);
          resolve(response);
          return;
        }
        reject(new Error(response.message || t('update.updateFailed', { type: label })));
      };

      xhr.onerror = () => reject(new Error(t('update.networkError', { type: label })));
      xhr.send(file);
    });
  }

  function restartAfterUpdate() {
    return fetch('/api/update/restart', { method: 'POST' })
      .then(Candle.parseJsonResponse)
      .then(({ ok, json }) => {
        if (!ok || json.status !== 'ok') {
          throw new Error(json.message || t('update.restartFailed'));
        }
        return json;
      })
      .catch((err) => {
        // A real restart can close the HTTP connection before the browser
        // receives JSON. Treat that as "restart requested" and wait for boot.
        if (err && /Failed to fetch|NetworkError|Load failed|network/i.test(err.message || '')) {
          return { status: 'ok', restarting: true, restartAssumed: true };
        }
        throw err;
      });
  }

  function uploadCombinedUpdate() {
    let firmwareFile;
    let filesystemFile;
    try {
      firmwareFile = selectedUpdateFile(els.firmwareFile, t('update.firmwareLabel'));
      filesystemFile = selectedUpdateFile(els.filesystemFile, t('update.littlefsLabel'));
    } catch (err) {
      setUpdateStatus(err.message, 'update-status--error');
      Candle.showToast(err.message, 'error');
      return;
    }

    setUpdateControlsDisabled(true);
    setUpdateProgress(0);
    setUpdateStatus(t('update.uploadingFirmware', { name: firmwareFile.name }));

    uploadBinaryUpdate('firmware', firmwareFile, 0, 50, t('update.firmwareType'))
      .then(() => {
        setUpdateStatus(t('update.uploadingLittlefs', { name: filesystemFile.name }));
        return uploadBinaryUpdate('filesystem', filesystemFile, 50, 100, t('update.filesystemType'));
      })
      .then(() => {
        setUpdateStatus(t('update.uploadedRestart'), 'update-status--ok');
        return restartAfterUpdate();
      })
      .then(() => {
        Candle.showRestartNotice(t('update.uploadedWaiting'));
        Candle.waitForDeviceAndReload();
      })
      .catch((err) => {
        setUpdateControlsDisabled(false);
        setUpdateStatus(err.message || t('update.failed'), 'update-status--error');
        Candle.showToast(err.message || t('update.failed'), 'error');
      });
  }

  function bind() {
    els.firmwareFile?.addEventListener('change', () => updateFileMeta(els.firmwareFile, els.firmwareFileMeta));
    els.filesystemFile?.addEventListener('change', () => updateFileMeta(els.filesystemFile, els.filesystemFileMeta));
    els.updateBtn?.addEventListener('click', uploadCombinedUpdate);
  }

  window.CandleUpdate = {
    bind
  };
}());
