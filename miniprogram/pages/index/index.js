const SERVICE_UUID = '0100583e-3e44-62a4-5e4d-627b905a538d';
const COMMAND_UUID = '0200583e-3e44-62a4-5e4d-627b905a538d';
const STATUS_UUID = '0300583e-3e44-62a4-5e4d-627b905a538d';
const HISTORY_UUID = '0400583e-3e44-62a4-5e4d-627b905a538d';
const DEVICE_NAME = 'ESP32C3-Haptic';
const SETTINGS_STORAGE_KEY = 'deviceSettings';

const HISTORY_TYPE_LABELS = {
  1: '开始久坐',
  2: '久坐完成',
  3: '提醒',
  4: '异常中断'
};
const RESET_REASON_LABELS = {
  1: '上电复位',
  2: '外部复位',
  3: '软件复位',
  4: '异常崩溃',
  5: '中断看门狗',
  6: '任务看门狗',
  7: '看门狗复位',
  8: '深度睡眠唤醒',
  9: '电压不足',
  10: 'SDIO 复位'
};

const VIBRATION_PRESETS = {
  blast: {
    1: { frequencyHz: 50, amplitude: 80 },
    2: { frequencyHz: 55, amplitude: 70 }
  },
  awkward: {
    1: { frequencyHz: 60, amplitude: 70 },
    2: { frequencyHz: 60, amplitude: 80 }
  }
};
const MANUAL_VIBRATION_EFFECTS = [
  { id: 'WEAK', name: '弱', durationMs: 1000 },
  { id: 'STRONG', name: '强', durationMs: 1000 },
  { id: 'LONG', name: '长', durationMs: 5000 }
];

function presetForLevel(kind, level) {
  return VIBRATION_PRESETS[kind][level] || VIBRATION_PRESETS[kind][1];
}

function levelForPreset(kind, frequencyHz, amplitude) {
  const presets = VIBRATION_PRESETS[kind];
  if (presets[1].frequencyHz === frequencyHz &&
      presets[1].amplitude === amplitude) return 1;
  if (presets[2].frequencyHz === frequencyHz &&
      presets[2].amplitude === amplitude) return 2;
  return 1;
}

function normalizeUuid(uuid) {
  return String(uuid || '').replace(/-/g, '').toUpperCase();
}

function isTargetDevice(device) {
  const names = [device.name, device.localName]
    .filter(Boolean)
    .map((name) => String(name).toLowerCase());
  const advertisesService = (device.advertisServiceUUIDs || []).some(
    (uuid) => normalizeUuid(uuid) === normalizeUuid(SERVICE_UUID)
  );
  return names.some((name) => name === DEVICE_NAME.toLowerCase()) ||
    advertisesService;
}

function callBluetoothApi(api, options) {
  return new Promise((resolve, reject) => {
    if (typeof wx[api] !== 'function') {
      reject(new Error(`当前微信版本不支持蓝牙接口 ${api}`));
      return;
    }
    wx[api]({
      ...options,
      success: resolve,
      fail: reject
    });
  });
}

function decodeAscii(buffer) {
  const bytes = new Uint8Array(buffer);
  let text = '';
  for (let i = 0; i < bytes.length; i += 1) {
    text += String.fromCharCode(bytes[i]);
  }
  return text;
}

function encodeAscii(text) {
  const bytes = new Uint8Array(text.length);
  for (let i = 0; i < text.length; i += 1) bytes[i] = text.charCodeAt(i);
  return bytes.buffer;
}

function formatDuration(seconds) {
  if (seconds < 60) return `${seconds}秒`;
  const totalMinutes = Math.floor(seconds / 60);
  const hours = Math.floor(totalMinutes / 60);
  const minutes = totalMinutes % 60;
  return hours > 0 ? `${hours}小时${minutes}分钟` : `${minutes}分钟`;
}

function formatDateTime(epoch) {
  if (!epoch || epoch <= 0) return '设备未对时';
  // The device stores the phone's local wall-clock time expressed as a UTC
  // timestamp (the CLOCK command already adds the phone's UTC offset so the
  // device's calendar-day rollover matches the phone's local date). Read it
  // back with the UTC accessors to avoid applying the phone's timezone twice.
  const date = new Date(epoch * 1000);
  const pad = (n) => (n < 10 ? `0${n}` : String(n));
  return `${date.getUTCFullYear()}-${pad(date.getUTCMonth() + 1)}-${pad(date.getUTCDate())} ` +
    `${pad(date.getUTCHours())}:${pad(date.getUTCMinutes())}`;
}

Page({
  data: {
    connected: false,
    connectedName: '',
    scanning: false,
    scanCooldown: false,
    scanFinished: false,
    devices: [],
    pressure: false,
    level1VibrationActive: false,
    level2VibrationActive: false,
    volume: 70,
    blastLevel: 1,
    awkwardLevel: 1,
    blastFrequencyHz: 50,
    motorFrequencyHz: 60,
    blastAmplitude: 80,
    motorAmplitude: 70,
    motorPwmCarrierHz: 20000,
    manualMotorAmplitude: 70,
    manualMotorFrequencyHz: 65,
    remindersEnabled: true,
    remindersSaved: true,
    remindersHint: '开关状态已保存到设备',
    powerBusy: false,
    sessionActive: false,
    leaveConfirming: false,
    sessionSeconds: 0,
    todaySittingSeconds: 0,
    todaySessions: 0,
    todayReminders: 0,
    lastSessionSeconds: 0,
    lastSessionReminders: 0,
    lastSessionResponded: false,
    lastSessionExists: false,
    sessionDuration: '0分钟',
    todayDuration: '0分钟',
    lastSessionDuration: '—',
    clockSynced: false,
    delays: [
      { name: '一级提醒 · 轻微屁', value: '30' },
      { name: '二级提醒 · 爆炸屁', value: '60' },
      { name: '三级提醒 · 尴尬屁', value: '90' }
    ],
    saving: false,
    settingsDirty: false,
    saveError: false,
    saveState: '设置保存到 ESP32-C3 后，重启仍会保留。',
    manualRunning: false,
    manualBusy: false,
    stopBusy: false,
    motorDirectRunning: false,
    motorDirectBusy: false,
    reminderTriggerBusy: false,
    manualVibrationEffects: MANUAL_VIBRATION_EFFECTS,
    historyList: [],
    historyCount: 0,
    historySyncing: false,
    historyNote: ''
  },

  onLoad() {
    this.deviceId = '';
    this.serviceId = '';
    this.commandId = '';
    this.statusId = '';
    this.historyId = '';
    this.expectedSettings = null;
    this.commandWriteChain = Promise.resolve();
    this.discoveryHandler = this.handleDeviceFound.bind(this);
    this.valueHandler = this.handleCharacteristicValue.bind(this);
    this.connectionHandler = this.handleConnectionState.bind(this);
    this.adapterStateHandler = this.handleAdapterStateChange.bind(this);
    wx.onBluetoothDeviceFound(this.discoveryHandler);
    wx.onBLECharacteristicValueChange(this.valueHandler);
    wx.onBLEConnectionStateChange(this.connectionHandler);
    wx.onBluetoothAdapterStateChange(this.adapterStateHandler);
    this.loadSettings();
    this.renderHistory(wx.getStorageSync('historyRecords') || []);
  },

  onUnload() {
    this.stopScanInternal();
    clearTimeout(this.scanTimeout);
    clearTimeout(this.scanCooldownTimer);
    clearTimeout(this.settingsTimeout);
    clearTimeout(this.manualStopTimer);
    wx.offBluetoothDeviceFound(this.discoveryHandler);
    wx.offBLECharacteristicValueChange(this.valueHandler);
    wx.offBLEConnectionStateChange(this.connectionHandler);
    wx.offBluetoothAdapterStateChange(this.adapterStateHandler);
    if (this.deviceId) {
      wx.closeBLEConnection({ deviceId: this.deviceId });
    }
    wx.closeBluetoothAdapter();
  },

  persistSettings() {
    wx.setStorageSync(SETTINGS_STORAGE_KEY, {
      volume: this.data.volume,
      delays: this.data.delays.map((item) => Number(item.value) || 0),
      blastFrequencyHz: this.data.blastFrequencyHz,
      motorFrequencyHz: this.data.motorFrequencyHz,
      blastAmplitude: this.data.blastAmplitude,
      motorAmplitude: this.data.motorAmplitude,
      motorPwmCarrierHz: this.data.motorPwmCarrierHz,
      manualMotorAmplitude: Number(this.data.manualMotorAmplitude),
      manualMotorFrequencyHz: Number(this.data.manualMotorFrequencyHz),
      blastLevel: this.data.blastLevel,
      awkwardLevel: this.data.awkwardLevel,
      remindersEnabled: this.data.remindersEnabled,
    });
  },

  loadSettings() {
    const saved = wx.getStorageSync(SETTINGS_STORAGE_KEY);
    if (!saved || typeof saved !== 'object') return;
    const clamp = (value, min, max, fallback) =>
      Number.isInteger(value) && value >= min && value <= max
        ? value
        : fallback;
    const delays = this.data.delays.map((item, index) => {
      const minutes = saved.delays && saved.delays[index];
      const value = Number.isInteger(minutes) && minutes >= 1 && minutes <= 1440
        ? minutes
        : (Number(item.value) || 30);
      return { ...item, value: String(value) };
    });
    const blastLevel = saved.blastLevel === 2 ? 2 : 1;
    const awkwardLevel = saved.awkwardLevel === 2 ? 2 : 1;
    this.setData({
      volume: clamp(saved.volume, 0, 100, this.data.volume),
      blastLevel,
      awkwardLevel,
      blastFrequencyHz: clamp(Number(saved.blastFrequencyHz), 40, 200,
        this.data.blastFrequencyHz),
      motorFrequencyHz: clamp(Number(saved.motorFrequencyHz), 40, 200,
        this.data.motorFrequencyHz),
      blastAmplitude: clamp(Number(saved.blastAmplitude), 1, 90,
        this.data.blastAmplitude),
      motorAmplitude: clamp(Number(saved.motorAmplitude), 1, 90,
        this.data.motorAmplitude),
      motorPwmCarrierHz: clamp(Number(saved.motorPwmCarrierHz), 5000, 30000,
        this.data.motorPwmCarrierHz),
      manualMotorAmplitude: clamp(Number(saved.manualMotorAmplitude), 1, 90,
        this.data.manualMotorAmplitude),
      manualMotorFrequencyHz: clamp(Number(saved.manualMotorFrequencyHz), 40, 200,
        this.data.manualMotorFrequencyHz),
      remindersEnabled: typeof saved.remindersEnabled === 'boolean'
        ? saved.remindersEnabled
        : this.data.remindersEnabled,
      delays
    });
  },

  async startScan() {
    if (this.data.scanning || this.data.connected || this.data.scanCooldown) return;
    this.setData({ scanning: true, scanCooldown: true, scanFinished: false, devices: [] });
    clearTimeout(this.scanCooldownTimer);
    this.scanCooldownTimer = setTimeout(() => {
      this.setData({ scanCooldown: false });
    }, 5000);
    try {
      await callBluetoothApi('openBluetoothAdapter');
      await callBluetoothApi('startBluetoothDevicesDiscovery', {
        allowDuplicatesKey: true
      });
      // 兜底：把微信已发现/缓存的设备也拉一次，避免断开重连后 onBluetoothDeviceFound 不再上报
      try {
        const cached = await callBluetoothApi('getBluetoothDevices');
        if (cached && cached.devices && cached.devices.length) {
          this.handleDeviceFound({ devices: cached.devices });
        }
      } catch (error) {
        // 忽略：个别平台不支持该接口，仍依赖 onBluetoothDeviceFound 上报
      }
      this.scanTimeout = setTimeout(() => {
        if (!this.data.scanning) return;
        this.stopScanInternal();
        this.setData({ scanFinished: true });
      }, 20000);
      wx.showToast({ title: '正在搜索设备', icon: 'none' });
    } catch (error) {
      this.setData({ scanning: false, scanFinished: true });
      if (error && error.errCode === 10001) {
        this.showBluetoothError('无法开始蓝牙搜索', {
          errMsg: '手机蓝牙未开启，请在系统设置中开启蓝牙后重试。'
        });
      } else {
        this.showBluetoothError('无法开始蓝牙搜索', error);
      }
    }
  },

  handleDeviceFound(result) {
    const found = result.devices || [];
    const devices = this.data.devices.slice();
    found.forEach((device) => {
      if (!device.deviceId) return;
      const existing = devices.find((item) => item.deviceId === device.deviceId);
      const name = device.name || device.localName || '';
      if (existing) {
        if (name && !existing.name) existing.name = name;
        existing.name = name || existing.name;
        return;
      }
      if (!isTargetDevice(device)) return;
      devices.push({
        deviceId: device.deviceId,
        name,
        isTarget: true
      });
    });
    this.setData({ devices });
    if (devices.length > 0) {
      this.stopScanInternal();
    }
  },

  stopScan() {
    this.stopScanInternal();
    this.setData({ scanFinished: true });
  },

  stopScanInternal() {
    clearTimeout(this.scanTimeout);
    if (this.data.scanning) {
      wx.stopBluetoothDevicesDiscovery({ fail: () => {} });
      this.setData({ scanning: false });
    }
  },

  async connectDevice(event) {
    const deviceId = event.currentTarget.dataset.deviceId;
    const name = event.currentTarget.dataset.name || '未命名蓝牙设备';
    if (!deviceId || this.data.connected) return;
    this.stopScanInternal();
    wx.showLoading({ title: '正在连接', mask: true });
    this.deviceId = deviceId;

    try {
      await callBluetoothApi('createBLEConnection', {
        deviceId,
        timeout: 12000
      });
      if (wx.getSystemInfoSync().platform === 'android' &&
          typeof wx.setBLEMTU === 'function') {
        try {
          await callBluetoothApi('setBLEMTU', { deviceId, mtu: 256 });
        } catch (error) {
          wx.showToast({
            title: '使用手机默认蓝牙数据长度',
            icon: 'none',
            duration: 1800
          });
        }
      }

      const servicesResult = await callBluetoothApi('getBLEDeviceServices', {
        deviceId
      });
      const service = (servicesResult.services || []).find(
        (item) => normalizeUuid(item.uuid) === normalizeUuid(SERVICE_UUID)
      );
      if (!service) throw new Error('设备未提供音频控制服务，请确认已烧录最新固件。');
      this.serviceId = service.uuid;

      const charsResult = await callBluetoothApi('getBLEDeviceCharacteristics', {
        deviceId,
        serviceId: this.serviceId
      });
      const characteristics = charsResult.characteristics || [];
      const command = characteristics.find(
        (item) => normalizeUuid(item.uuid) === normalizeUuid(COMMAND_UUID)
      );
      const status = characteristics.find(
        (item) => normalizeUuid(item.uuid) === normalizeUuid(STATUS_UUID)
      );
      const history = characteristics.find(
        (item) => normalizeUuid(item.uuid) === normalizeUuid(HISTORY_UUID)
      );
      if (!command || !status || !history) {
        throw new Error('设备特征值不完整，请确认已烧录最新固件。');
      }
      this.commandId = command.uuid;
      this.statusId = status.uuid;
      this.historyId = history.uuid;

      await callBluetoothApi('notifyBLECharacteristicValueChange', {
        deviceId,
        serviceId: this.serviceId,
        characteristicId: this.statusId,
        state: true
      });
      await callBluetoothApi('notifyBLECharacteristicValueChange', {
        deviceId,
        serviceId: this.serviceId,
        characteristicId: this.historyId,
        state: true
      });
      this.setData({
        connected: true,
        connectedName: name,
        saveState: '已连接，正在读取设备设置…',
        saveError: false
      });
      await callBluetoothApi('readBLECharacteristicValue', {
        deviceId,
        serviceId: this.serviceId,
        characteristicId: this.statusId
      });
      const localEpochSeconds = Math.floor(Date.now() / 1000) -
        new Date().getTimezoneOffset() * 60;
      try {
        await this.writeCommand(`CLOCK ${localEpochSeconds}`);
      } catch (error) {
        this.setData({
          saveState: `已连接，但手机日期同步失败：${error.errMsg || error.message || '蓝牙写入失败'}`,
          saveError: true
        });
      }
      wx.hideLoading();
      wx.showToast({ title: '蓝牙已连接', icon: 'success' });
      this.startHistorySync();
    } catch (error) {
      wx.hideLoading();
      if (this.deviceId) {
        wx.closeBLEConnection({ deviceId: this.deviceId });
      }
      this.clearConnection();
      this.showBluetoothError('连接设备失败', error);
    }
  },

  handleCharacteristicValue(result) {
    if (result.deviceId !== this.deviceId) return;
    if (normalizeUuid(result.characteristicId) === normalizeUuid(this.historyId)) {
      this.handleHistoryValue(result);
      return;
    }
    if (normalizeUuid(result.characteristicId) !== normalizeUuid(this.statusId)) {
      return;
    }
    try {
      const raw = JSON.parse(decodeAscii(result.value));
      const vibrationLevel1 = raw.v1 === undefined ? 0 : raw.v1;
      const vibrationLevel2 = raw.v2 === undefined ? 0 : raw.v2;
      if (![raw.p, raw.v, raw.ok, raw.on, raw.k, raw.a, raw.lv, raw.s, raw.t,
        raw.n, raw.r, raw.l, raw.q, raw.x, raw.c, raw.bh, raw.mh, raw.mk,
        raw.ba, raw.ma, vibrationLevel1, vibrationLevel2]
        .every(Number.isInteger) ||
          !Array.isArray(raw.d) || raw.d.length !== 3 ||
          !raw.d.every((value) =>
            Number.isInteger(value) && value >= 60000 && value <= 86400000
          ) ||
          raw.v < 0 || raw.v > 100 ||
          raw.p < 0 || raw.p > 1 ||
          raw.on < 0 || raw.on > 1 ||
          raw.k < 0 || raw.k > 1 ||
          raw.a < 0 || raw.a > 1 ||
          raw.lv < 0 || raw.lv > 1 ||
          raw.ok < 0 || raw.ok > 1 ||
          raw.bh < 40 || raw.bh > 200 ||
          raw.mh < 40 || raw.mh > 200 ||
          raw.ba < 1 || raw.ba > 90 ||
          raw.ma < 1 || raw.ma > 90 ||
          (raw.pc !== undefined &&
            (!Number.isInteger(raw.pc) || raw.pc < 5000 || raw.pc > 30000)) ||
          (raw.mr !== undefined &&
            (!Number.isInteger(raw.mr) || raw.mr < 0 || raw.mr > 1)) ||
          raw.mk < 0 || raw.mk > 1 ||
          vibrationLevel1 < 0 || vibrationLevel1 > 1 ||
          vibrationLevel2 < 0 || vibrationLevel2 > 1 ||
          raw.x < 0 || raw.x > 1) {
        throw new Error('设备状态格式不正确');
      }
      const status = {
        pressure: raw.p === 1,
        volume: raw.v,
        delays: raw.d,
        saved: raw.ok === 1,
        enabled: raw.on === 1,
        enabledSaved: raw.k === 1,
        sessionActive: raw.a === 1,
        leaveConfirming: raw.lv === 1,
        sessionSeconds: raw.s,
        todaySittingSeconds: raw.t,
        todaySessions: raw.n,
        todayReminders: raw.r,
        lastSessionSeconds: raw.l,
        lastSessionReminders: raw.q,
        lastSessionResponded: raw.x === 1,
        clockSynced: raw.c === 1,
        blastFrequencyHz: raw.bh,
        motorFrequencyHz: raw.mh,
        blastAmplitude: raw.ba,
        motorAmplitude: raw.ma,
        motorPwmCarrierHz: raw.pc === undefined ? 20000 : raw.pc,
        motorDirectRunning: raw.mr === 1,
        motorSaved: raw.mk === 1,
        level1VibrationActive: vibrationLevel1 === 1,
        level2VibrationActive: vibrationLevel2 === 1
      };
      const delays = status.delays.map((value, index) => ({
        name: this.data.delays[index].name,
        value: String(value / 60000)
      }));
      const expected = this.expectedSettings;
      const matchesExpected = expected &&
        status.volume === expected.volume &&
        status.blastFrequencyHz === expected.blastFrequencyHz &&
        status.motorFrequencyHz === expected.motorFrequencyHz &&
        status.blastAmplitude === expected.blastAmplitude &&
        status.motorAmplitude === expected.motorAmplitude &&
        status.motorPwmCarrierHz === expected.motorPwmCarrierHz &&
        status.delays.every((value, index) => value === expected.delays[index]);
      const applySettings = matchesExpected || !this.data.settingsDirty;
      const awaitingSettings = expected && !matchesExpected;
      this.setData({
        pressure: status.pressure,
        level1VibrationActive: status.level1VibrationActive,
        level2VibrationActive: status.level2VibrationActive,
        remindersEnabled: status.enabled,
        remindersSaved: status.enabledSaved,
        remindersHint: status.enabledSaved
          ? (status.enabled
            ? '已开启 · 久坐达到设定时间后提醒；设备启动时播放提示音'
            : '已静音 · 久坐时长与轮次统计继续')
          : '开关已生效，但保存到设备失败',
        sessionActive: status.sessionActive,
        leaveConfirming: status.leaveConfirming,
        sessionSeconds: status.sessionSeconds,
        todaySittingSeconds: status.todaySittingSeconds,
        todaySessions: status.todaySessions,
        todayReminders: status.todayReminders,
        lastSessionSeconds: status.lastSessionSeconds,
        lastSessionReminders: status.lastSessionReminders,
        lastSessionResponded: status.lastSessionResponded,
        lastSessionExists: status.todaySessions > 0,
        sessionDuration: formatDuration(status.sessionSeconds),
        todayDuration: formatDuration(status.todaySittingSeconds),
        lastSessionDuration: status.todaySessions > 0
          ? formatDuration(status.lastSessionSeconds)
          : '—',
        clockSynced: status.clockSynced,
        volume: applySettings ? status.volume : this.data.volume,
        motorFrequencyHz: applySettings
          ? status.motorFrequencyHz
          : this.data.motorFrequencyHz,
        blastFrequencyHz: applySettings
          ? status.blastFrequencyHz
          : this.data.blastFrequencyHz,
        blastAmplitude: applySettings
          ? status.blastAmplitude
          : this.data.blastAmplitude,
        motorAmplitude: applySettings
          ? status.motorAmplitude
          : this.data.motorAmplitude,
        motorPwmCarrierHz: applySettings
          ? status.motorPwmCarrierHz
          : this.data.motorPwmCarrierHz,
        motorDirectRunning: status.motorDirectRunning,
        blastLevel: applySettings
          ? levelForPreset('blast', status.blastFrequencyHz, status.blastAmplitude)
          : this.data.blastLevel,
        awkwardLevel: applySettings
          ? levelForPreset('awkward', status.motorFrequencyHz, status.motorAmplitude)
          : this.data.awkwardLevel,
        delays: applySettings ? delays : this.data.delays,
        saveState: awaitingSettings
          ? '设置已发送，等待设备写入 Flash 确认…'
          : (!status.enabledSaved
            ? '提醒声音开关已生效，但保存到设备失败。'
            : (status.saved && status.motorSaved
              ? '设备设置已保存。'
              : '设置已生效，但部分设置保存到 Flash 失败。')),
        saveError: awaitingSettings
          ? false
          : (!status.saved || !status.enabledSaved || !status.motorSaved)
      });
      this.persistSettings();
      if (matchesExpected) {
        this.expectedSettings = null;
        clearTimeout(this.settingsTimeout);
        this.setData({ saving: false, settingsDirty: false });
        wx.showToast({
          title: status.saved ? '设置已保存' : 'Flash 保存失败',
          icon: status.saved ? 'success' : 'none'
        });
      }
    } catch (error) {
      this.setData({
        saveState: `读取设备状态失败：${error.message}`,
        saveError: true
      });
    }
  },

  handleConnectionState(result) {
    if (result.deviceId === this.deviceId && !result.connected) {
      this.clearConnection();
      this.setData({
        saveState: '蓝牙已断开，请重新搜索并连接。',
        saveError: true
      });
    }
  },

  handleAdapterStateChange(res) {
    if (res.available) return;
    // 手机蓝牙被关闭：停止搜索、清掉连接状态并提示
    if (this.data.scanning) {
      this.stopScanInternal();
    }
    if (this.deviceId || this.data.connected) {
      this.clearConnection();
    }
    this.setData({
      saveState: '手机蓝牙已关闭，请先在系统设置中开启蓝牙。',
      saveError: true
    });
  },

  startHistorySync() {
    this.pendingHistory = [];
    this.historySyncingSeq = wx.getStorageSync('lastSyncedSeq') || 0;
    this.setData({ historySyncing: true, historyNote: '正在同步使用记录…' });
    this.writeCommand(`SYNC ${this.historySyncingSeq}`).catch(() => {
      this.setData({
        historySyncing: false,
        historyNote: '同步请求发送失败，断开重连后重试'
      });
    });
  },

  handleHistoryValue(result) {
    let raw;
    try {
      raw = JSON.parse(decodeAscii(result.value));
    } catch (error) {
      return;
    }
    if (raw.done === 1) {
      this.finishHistorySync(raw.max);
      return;
    }
    if (!Number.isInteger(raw.s) || raw.s < 1 || !Number.isInteger(raw.t)) {
      return;
    }
    const record = {
      seq: raw.s,
      type: raw.t,
      online: raw.o === 1,
      epoch: raw.e,
      uptime: raw.u,
      duration: raw.d,
      reminders: raw.r,
      responded: raw.x,
      level: raw.l,
      sessionSeq: raw.k,
      reason: raw.g
    };
    if (this.data.historySyncing) {
      this.pendingHistory = this.pendingHistory || [];
      this.pendingHistory.push(record);
    } else {
      // Real-time push: a record arrived outside a SYNC batch, so fold it in now.
      this.mergeAndStore([record]);
    }
  },

  mergeAndStore(records, maxSeq) {
    let lastSeq = this.historySyncingSeq || 0;
    if (Number.isInteger(maxSeq) && maxSeq > lastSeq) lastSeq = maxSeq;
    records.forEach((record) => {
      if (record.seq > lastSeq) lastSeq = record.seq;
    });

    const existing = wx.getStorageSync('historyRecords') || [];
    const bySeq = {};
    existing.forEach((record) => { bySeq[record.seq] = record; });
    records.forEach((record) => { bySeq[record.seq] = record; });
    const merged = Object.keys(bySeq)
      .map((key) => bySeq[key])
      .sort((a, b) => a.seq - b.seq);

    wx.setStorageSync('historyRecords', merged);
    this.historySyncingSeq = lastSeq;
    wx.setStorageSync('lastSyncedSeq', lastSeq);
    this.renderHistory(merged);
    this.writeCommand(`ACK ${lastSeq}`).catch(() => {});
  },

  finishHistorySync(maxSeq) {
    const records = this.pendingHistory || [];
    this.pendingHistory = [];
    this.mergeAndStore(records, maxSeq);
    this.setData({
      historySyncing: false,
      historyNote: `已同步 ${this.data.historyCount} 条记录`
    });
  },

  historySummary(record) {
    if (record.type === 2) {
      const response = record.reminders > 0 ? '已响应' : '无提醒';
      return `${formatDuration(record.duration)} · ${record.reminders} 次提醒 · ${response}`;
    }
    if (record.type === 4) {
      const reason = RESET_REASON_LABELS[record.reason] || '未知原因';
      return `${reason} · 已坐 ${formatDuration(record.duration)}`;
    }
    if (record.type === 3) {
      return `第 ${record.level} 级提醒`;
    }
    if (record.type === 1) {
      return '开始久坐';
    }
    return '';
  },

  renderHistory(records) {
    const list = records
      .slice()
      .reverse()
      .slice(0, 30)
      .map((record) => {
        const online = record.online === true || record.online === 1;
        const hasTime = record.epoch > 0;
        return {
          ...record,
          online,
          typeLabel: HISTORY_TYPE_LABELS[record.type] || '记录',
          timeText: hasTime ? formatDateTime(record.epoch) : '未同步时间',
          summary: this.historySummary(record)
        };
      });
    this.setData({
      historyList: list,
      historyCount: records.length
    });
  },

  clearHistory() {
    wx.showModal({
      title: '清空使用记录',
      content: '将清空设备与手机上的全部使用记录，且不可恢复。',
      confirmText: '清空',
      confirmColor: '#d9534f',
      success: (result) => {
        if (!result.confirm) return;
        this.writeCommand('CLEAR').then(() => {
          wx.removeStorageSync('historyRecords');
          wx.removeStorageSync('lastSyncedSeq');
          this.historySyncingSeq = 0;
          this.pendingHistory = [];
          this.renderHistory([]);
          this.setData({ historyNote: '使用记录已清空' });
        }).catch(() => {
          this.setData({ historyNote: '清空请求发送失败，请重试' });
        });
      }
    });
  },

  onVolumeChanging(event) {
    this.setData({ volume: Number(event.detail.value), settingsDirty: true });
    this.persistSettings();
  },

  onVolumeChange(event) {
    this.setData({ volume: Number(event.detail.value), settingsDirty: true });
    this.persistSettings();
  },

  onDelayInput(event) {
    const index = Number(event.currentTarget.dataset.index);
    const delays = this.data.delays.slice();
    delays[index] = { ...delays[index], value: event.detail.value };
    this.setData({ delays, settingsDirty: true });
    this.persistSettings();
  },

  onMotorSettingInput(event) {
    const field = event.currentTarget.dataset.field;
    if (!['blastAmplitude', 'blastFrequencyHz', 'motorAmplitude',
      'motorFrequencyHz', 'motorPwmCarrierHz', 'manualMotorAmplitude',
      'manualMotorFrequencyHz'].includes(field)) return;
    this.setData({
      [field]: event.detail.value,
      settingsDirty: true
    });
    this.persistSettings();
  },

  onVibrationLevelChange(event) {
    if (!this.data.connected) return;
    const kind = event.currentTarget.dataset.vibration;
    const level = Number(event.currentTarget.dataset.level);
    const preset = presetForLevel(kind, level);
    if (kind === 'blast') {
      this.setData({
        blastLevel: level,
        blastFrequencyHz: preset.frequencyHz,
        blastAmplitude: preset.amplitude,
        settingsDirty: true
      });
    } else if (kind === 'awkward') {
      this.setData({
        awkwardLevel: level,
        motorFrequencyHz: preset.frequencyHz,
        motorAmplitude: preset.amplitude,
        settingsDirty: true
      });
    }
    this.persistSettings();
  },

  formatManualDuration(ms) {
    const seconds = ms / 1000;
    const text = Number.isInteger(seconds) ? String(seconds) : seconds.toFixed(1);
    return `${text} 秒`;
  },

  onManualIntensityChanging(event) {
    this.setData({ manualIntensity: Number(event.detail.value) });
    this.persistSettings();
  },

  onManualIntensityChange(event) {
    this.setData({ manualIntensity: Number(event.detail.value) });
    this.persistSettings();
  },

  onManualFrequencyChange(event) {
    this.setData({ manualFrequencyHz: Number(event.detail.value) });
    this.persistSettings();
  },

  onManualDurationChanging(event) {
    const manualDurationMs = Number(event.detail.value);
    this.setData({
      manualDurationMs,
      manualDurationText: this.formatManualDuration(manualDurationMs)
    });
    this.persistSettings();
  },

  onManualDurationChange(event) {
    const manualDurationMs = Number(event.detail.value);
    this.setData({
      manualDurationMs,
      manualDurationText: this.formatManualDuration(manualDurationMs)
    });
    this.persistSettings();
  },

  async triggerVibration() {
    if (this.data.stopBusy || this.data.manualBusy || this.data.manualRunning ||
        this.data.motorDirectRunning || !this.data.connected) {
      return;
    }
    const amplitude = this.data.manualIntensity;
    const frequency = this.data.manualFrequencyHz;
    const durationMs = this.data.manualDurationMs;
    this.setData({ manualBusy: true });
    try {
      await this.writeCommand(`VIBRATE ${amplitude} ${frequency} ${durationMs}`);
      this.setData({ manualRunning: true });
      clearTimeout(this.manualStopTimer);
      this.manualStopTimer = setTimeout(() => {
        this.setData({ manualRunning: false });
      }, durationMs + 300);
    } catch (error) {
      wx.showToast({ title: '触发失败：请确认已连接', icon: 'none' });
    } finally {
      this.setData({ manualBusy: false });
    }
  },

  async triggerVibrationEffect(event) {
    if (this.data.stopBusy || this.data.manualBusy || this.data.manualRunning ||
        this.data.motorDirectRunning || !this.data.connected) {
      return;
    }
    const effectId = event.currentTarget.dataset.effect;
    const effect = MANUAL_VIBRATION_EFFECTS.find((item) => item.id === effectId);
    if (!effect) {
      wx.showToast({ title: '未知振动效果', icon: 'none' });
      return;
    }
    this.setData({ manualBusy: true });
    try {
      await this.writeCommand(`VIBRATE ${effect.id}`);
      this.setData({ manualRunning: true });
      clearTimeout(this.manualStopTimer);
      this.manualStopTimer = setTimeout(() => {
        this.setData({ manualRunning: false });
      }, effect.durationMs + 300);
    } catch (error) {
      wx.showToast({ title: '触发失败：请确认已连接', icon: 'none' });
    } finally {
      this.setData({ manualBusy: false });
    }
  },

  async triggerReminder(event) {
    if (this.data.stopBusy || this.data.reminderTriggerBusy ||
        this.data.manualRunning ||
        this.data.motorDirectRunning || !this.data.connected) {
      return;
    }
    const level = Number(event.currentTarget.dataset.level);
    if (level < 1 || level > 3) {
      wx.showToast({ title: '未知提醒级别', icon: 'none' });
      return;
    }

    this.setData({ reminderTriggerBusy: true });
    try {
      await this.writeCommand(`REMINDER ${level}`);
      wx.showToast({
        title: `已触发${['', '一段', '二段', '三段'][level]}提醒`,
        icon: 'success'
      });
    } catch (error) {
      const detail = error.errMsg || error.message || '蓝牙写入失败';
      this.setData({
        saveState: `阶段提醒触发失败：${detail}。请确认设备已烧录支持 REMINDER 命令的新固件。`,
        saveError: true
      });
      wx.showModal({
        title: '阶段提醒触发失败',
        content: `蓝牙命令 REMINDER ${level} 写入失败：${detail}\n\n请确认 ESP32 已烧录支持独立触发阶段提醒的新固件，并保持蓝牙连接。`,
        showCancel: false
      });
    } finally {
      this.setData({ reminderTriggerBusy: false });
    }
  },

  async stopReminderAndVibration() {
    if (!this.data.connected || this.data.stopBusy || this.data.motorDirectBusy) return;
    this.setData({ stopBusy: true });
    try {
      await this.writeCommand('VIBRATE STOP');
    } catch (error) {
      wx.showToast({ title: '停止失败：请确认已连接', icon: 'none' });
      return;
    } finally {
      this.setData({ stopBusy: false });
    }
    clearTimeout(this.manualStopTimer);
    this.setData({ manualRunning: false, motorDirectRunning: false });
  },

  async startMotorDirect() {
    if (!this.data.connected || this.data.stopBusy ||
        this.data.motorDirectBusy ||
        this.data.motorDirectRunning) return;
    const amplitude = Number(this.data.manualMotorAmplitude);
    const frequencyHz = Number(this.data.manualMotorFrequencyHz);
    const carrierHz = Number(this.data.motorPwmCarrierHz);
    if (!Number.isInteger(amplitude) || amplitude < 1 || amplitude > 90 ||
        !Number.isInteger(frequencyHz) || frequencyHz < 40 ||
        frequencyHz > 200 || !Number.isInteger(carrierHz) ||
        carrierHz < 5000 || carrierHz > 30000) {
      wx.showToast({ title: '请检查振幅、频率和 PWM 载波范围', icon: 'none' });
      return;
    }
    this.setData({ motorDirectBusy: true });
    try {
      await this.writeCommand(
        `MOTOR_RUN ON ${amplitude} ${frequencyHz} ${carrierHz}`
      );
      this.setData({ motorDirectRunning: true });
    } catch (error) {
      wx.showToast({ title: '马达启动失败：请确认已连接', icon: 'none' });
    } finally {
      this.setData({ motorDirectBusy: false });
    }
  },

  async writeCommand(command) {
    const write = async () => {
      if (!this.deviceId || !this.commandId) {
        throw new Error('蓝牙尚未连接，请先连接设备。');
      }
      const framedCommand = `@${command}\n`;
      const chunkSize = 20;
      for (let offset = 0; offset < framedCommand.length; offset += chunkSize) {
        await callBluetoothApi('writeBLECharacteristicValue', {
          deviceId: this.deviceId,
          serviceId: this.serviceId,
          characteristicId: this.commandId,
          value: encodeAscii(framedCommand.slice(offset, offset + chunkSize)),
          writeType: 'write'
        });
      }
    };
    const pending = this.commandWriteChain.then(write, write);
    this.commandWriteChain = pending.catch(() => {});
    return pending;
  },

  async toggleReminders() {
    if (this.data.powerBusy || !this.data.connected) return;
    const enabled = !this.data.remindersEnabled;
    this.setData({
      powerBusy: true,
      saveError: false,
      remindersEnabled: enabled
    });
    this.persistSettings();
    try {
      await this.writeCommand(`POWER ${enabled ? 'ON' : 'OFF'}`);
      this.setData({
        powerBusy: false,
        saveState: enabled
          ? '提醒声音已开启，久坐提醒已生效。'
          : '提醒声音已关闭，久坐监测仍继续。'
      });
    } catch (error) {
      this.setData({
        powerBusy: false,
        saveError: true,
        saveState: `开关设置失败：${error.errMsg || error.message || '蓝牙写入失败'}`
      });
    }
  },

  async saveSettings() {
    const delayMinutes = this.data.delays.map((item) => {
      const value = item.value.trim();
      return value === '' ? NaN : Number(value);
    });
    if (delayMinutes.some((value) =>
      !Number.isInteger(value) || value < 1 || value > 1440
    ) || delayMinutes[0] > delayMinutes[1] ||
        delayMinutes[1] > delayMinutes[2]) {
      this.setData({
        saveState: '触发时间需为 1–1440 分钟，并按音效顺序不得递减。',
        saveError: true
      });
      return;
    }

    const delays = delayMinutes.map((value) => value * 60000);
    const settings = {
      volume: this.data.volume,
      delays,
      blastFrequencyHz: Number(this.data.blastFrequencyHz),
      motorFrequencyHz: Number(this.data.motorFrequencyHz),
      blastAmplitude: Number(this.data.blastAmplitude),
      motorAmplitude: Number(this.data.motorAmplitude),
      motorPwmCarrierHz: Number(this.data.motorPwmCarrierHz)
    };
    if (!Number.isInteger(settings.volume) ||
        settings.volume < 0 || settings.volume > 100) {
      this.setData({
        saveState: '音量需在 0–100% 之间。',
        saveError: true
      });
      return;
    }
    if (!Number.isInteger(settings.blastFrequencyHz) ||
        settings.blastFrequencyHz < 40 ||
        settings.blastFrequencyHz > 200 ||
        !Number.isInteger(settings.motorFrequencyHz) ||
        settings.motorFrequencyHz < 40 || settings.motorFrequencyHz > 200) {
      this.setData({
        saveState: '两级马达振动频率均需在 40–200 Hz 之间。',
        saveError: true
      });
      return;
    }
    if (!Number.isInteger(settings.blastAmplitude) ||
        settings.blastAmplitude < 1 || settings.blastAmplitude > 90 ||
        !Number.isInteger(settings.motorAmplitude) ||
        settings.motorAmplitude < 1 || settings.motorAmplitude > 90) {
      this.setData({
        saveState: '两级马达振幅均需在 1–90% 之间。',
        saveError: true
      });
      return;
    }
    if (!Number.isInteger(settings.motorPwmCarrierHz) ||
        settings.motorPwmCarrierHz < 5000 ||
        settings.motorPwmCarrierHz > 30000) {
      this.setData({
        saveState: 'PWM 载波频率需在 5000–30000 Hz 之间。',
        saveError: true
      });
      return;
    }
    const command = `AUDIO ${settings.volume} ${delays.join(' ')}`;
    this.expectedSettings = settings;
    this.setData({
      saving: true,
      settingsDirty: true,
      saveError: false,
      saveState: '设置已发送，等待设备写入 Flash 确认…'
    });
    this.settingsTimeout = setTimeout(() => {
      if (this.expectedSettings === settings) {
        this.expectedSettings = null;
        this.setData({
          saving: false,
          saveError: true,
          saveState: '等待设备确认超时；请检查蓝牙连接并重试。'
        });
      }
    }, 10000);
    try {
      await this.writeCommand(command);
      await this.writeCommand(
        `MOTOR ${settings.blastAmplitude} ${settings.blastFrequencyHz} ` +
        `${settings.motorAmplitude} ${settings.motorFrequencyHz} ` +
        `${settings.motorPwmCarrierHz}`
      );
    } catch (error) {
      this.expectedSettings = null;
      clearTimeout(this.settingsTimeout);
      this.setData({
        saving: false,
        saveError: true,
        saveState: `发送失败：${error.errMsg || error.message || '蓝牙写入失败'}`
      });
    }
  },

  // 断开时只关闭 BLE 连接、保留蓝牙适配器（与参考实现一致），
  // 避免频繁 close/open 适配器造成状态错乱而弹出“无法开始蓝牙搜索”。
  async disconnect() {
    const deviceId = this.deviceId;
    if (deviceId) {
      try {
        if (this.data.motorDirectRunning) {
          await this.writeCommand('MOTOR_RUN OFF');
        }
        await callBluetoothApi('closeBLEConnection', { deviceId });
      } catch (error) {
        this.showBluetoothError('断开蓝牙失败', error);
      }
    }
    this.clearConnection();
  },

  clearConnection() {
    this.deviceId = '';
    this.serviceId = '';
    this.commandId = '';
    this.statusId = '';
    this.historyId = '';
    this.expectedSettings = null;
    clearTimeout(this.settingsTimeout);
    clearTimeout(this.manualStopTimer);
    this.setData({
      connected: false,
      connectedName: '',
      level1VibrationActive: false,
      level2VibrationActive: false,
      saving: false,
      settingsDirty: false,
      manualRunning: false,
      manualBusy: false,
      motorDirectRunning: false,
      motorDirectBusy: false,
      reminderTriggerBusy: false,
      historySyncing: false
    });
  },

  showBluetoothError(title, error) {
    const detail = error && (error.errMsg || error.message)
      ? (error.errMsg || error.message)
      : '请确认手机蓝牙已开启，并允许微信使用蓝牙。';
    this.setData({
      saveState: `${title}：${detail}`,
      saveError: true
    });
    wx.showModal({
      title,
      content: `${detail}\n\n请确认 ESP32-C3 已上电、手机在附近，并且设备固件正在广播。`,
      showCancel: false
    });
  }
});
