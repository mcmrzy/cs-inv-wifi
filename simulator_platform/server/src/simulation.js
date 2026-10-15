'use strict';
/*
 * simulation.js — 模拟引擎：调度被认领设备，每分钟步进一次
 *
 * 每设备一个实例：weather -> pv -> load -> battery -> grid -> 快照
 * 快照同时：(1) 写 store（本地留存）(2) 交 MQTT 发布 heartbeat（180s 节拍 + 突变即时）。
 *
 * v2 扩展：_meta 暴露完整 30+ 字段供电站详情页消费。
 */
const { createWeatherFor } = require('./algorithms/weather');
const { createPv } = require('./algorithms/pv');
const { createLoad } = require('./algorithms/load');
const { createBattery } = require('./algorithms/battery');
const { buildSnapshot } = require('./algorithms/grid');
const { WeatherProvider } = require('./weather-provider');
const store = require('./store');

const STEP_MS = 60 * 1000; // 每分钟
const HEARTBEAT_PERIOD_S = 180; // 对齐固件
const RATED_W = 6200;

class DeviceSim {
  constructor(dev, weatherProvider) {
    this.dev = dev;
    this._wprov = weatherProvider || null;
    this._city = require('./geo').getCity(dev.city_code);
    this.weather = createWeatherFor(this._city);
    this.pv = createPv(RATED_W);
    this.load = createLoad((this._city && this._city.climate) || { tempC: 16 });
    this.battery = createBattery({ nominalWh: 9600 }); // 48V 200Ah
    this.eng = { EGen_today: 0, EGen_total: 0, Epv_today: 0, Epv_total: 0,
                 Eac_chrToday: 0, Eac_chrTotal: 0, Ebat_dischrToday: 0, Ebat_chrToday: 0,
                 Eac_dischrToday: 0, loadToday: 0, loadTotal: 0 };
    this.lastHeartbeat = 0;
    this.stepCount = 0;
    this._realWeather = null;
    this._realFetchAt = 0;
    this._pendingWeather = null; // 防止并发拉取 Open-Meteo
    this._realCity = null;

    // --- 控制参数模型（对齐协议 9.2 的 42 键，可被命令修改） ---
    this.ctrl = {
      set_output_priority: 1,            // 0=SolarFirst 1=BatteryFirst 2=UTI
      set_max_charge_current: 30,        // A (×0.1)
      set_ac_volt_range: 0,              // 枚举 0-2
      set_battery_capacity: 200,         // Ah
      set_battery_type: 0,               // 枚举 0-2 (0=LiFePO4)
      set_overload_restart: 1,           // bool
      set_high_temp_restart: 1,          // bool
      set_output_voltage: 230,           // V (×0.1)
      set_output_frequency: 50,          // Hz (×0.01)
      set_master_slave: 0,               // 枚举 0-1
      set_ac_charge_current: 0,          // A (×0.1)
      set_low_volt_return_utl: 48,       // V
      set_high_volt_return_bat: 52,      // V
      set_charge_priority: 0,            // 枚举 0-2
      set_alarm_control: 0,              // 位掩码
      set_backlight_ctrl: 2,             // 枚举 0-3
      set_power_shutdown_alarm: 1,       // bool
      set_overload_use_city_power: 0,    // bool
      set_max_discharge_current: 30,     // A
      set_max_chg_curr: 30,              // A
      set_recover_threshold_volt: 50,    // V
      set_solar_power_balance: 1,        // bool
      set_ac_output_mode: 0,             // 枚举 0-2
      set_li_bat_material: 0,            // 枚举 0-1
      set_cell_serial_lifepo4: 16,       // 节
      set_cell_serial_li_nmc: 16,        // 节
      set_equalize_enable: 0,            // bool
      set_equalize_voltage: 55,          // V
      set_equalize_time: 60,             // min
      set_equalize_timeout: 120,         // min
      set_equalize_interval: 7,          // day
      set_equalize_activate: 0,          // bool
      set_charge_time: 0,                // min
      set_close_charge_time: 0,          // min
      set_gen_start_voltage: 44,         // V
      set_gen_stop_voltage: 52,          // V
      set_soc_back_utl: 30,              // %
      set_soc_back_bat: 40,              // %
      set_soc_back_gen: 50,              // %
      set_soc_close_gen: 70,             // %
      set_soc_cutoff: 20,                // %
      set_buzzer: 1,                     // bool
      // 内部非协议状态
      power_on: true,                    // 开机/关机
    };
    this._ctrlRevision = 0;

    // --- 告警 / 故障状态 ---
    this._alarms = []; // [{code, name, level, ts}]
    this._cmdLog = []; // [{ts, cmd, result, applied, revision}]
  }

  async _refreshRealWeather() {
    if (!this._wprov || !this._city) return;
    const now = Date.now();
    if (now - this._realFetchAt < this._wprov.ttl) return;

    // 若有正在进行的拉取，等待它完成（避免同一 step 内多次并发请求）
    if (this._pendingWeather) {
      const result = await this._pendingWeather;
      if (result !== undefined) this._realWeather = result;
      this._pendingWeather = null;
      this._realFetchAt = Date.now();
      return;
    }

    // 启动新拉取，保存 promise 直到完成
    this._realFetchAt = now;
    this._pendingWeather = this._wprov.ensure(this._city.lat, this._city.lng)
      .then(data => { this._realWeather = data; this._pendingWeather = null; return data; })
      .catch(err => { console.warn(`[weather] ${this.dev.sn} fetch failed: ${err.message}`); this._pendingWeather = null; });
  }

  /** 应用控制命令（MQTT 下发或 API 调用），返回 applied_value */
  applyCmd(action, value) {
    this._ctrlRevision++;
    const rev = this._ctrlRevision;
    let applied = null;
    let result = 'OK';
    let err = 0;

    // 42 键控制命令的范围定义：{key, min, max, scale}（value 为工程单位）
    const RANGES = {
      set_output_priority: [0, 2], set_max_charge_current: [0, 60],
      set_ac_volt_range: [0, 2], set_battery_capacity: [0, 2000],
      set_battery_type: [0, 2], set_overload_restart: [0, 1],
      set_high_temp_restart: [0, 1], set_output_voltage: [200, 250],
      set_output_frequency: [45, 55], set_master_slave: [0, 1],
      set_ac_charge_current: [0, 150], set_low_volt_return_utl: [40, 60],
      set_high_volt_return_bat: [40, 60], set_charge_priority: [0, 2],
      set_alarm_control: [0, 255], set_backlight_ctrl: [0, 3],
      set_power_shutdown_alarm: [0, 1], set_overload_use_city_power: [0, 1],
      set_max_discharge_current: [0, 150], set_max_chg_curr: [0, 150],
      set_recover_threshold_volt: [40, 60], set_solar_power_balance: [0, 1],
      set_ac_output_mode: [0, 2], set_li_bat_material: [0, 1],
      set_cell_serial_lifepo4: [4, 32], set_cell_serial_li_nmc: [4, 32],
      set_equalize_enable: [0, 1], set_equalize_voltage: [40, 65],
      set_equalize_time: [0, 720], set_equalize_timeout: [0, 1440],
      set_equalize_interval: [0, 90], set_equalize_activate: [0, 1],
      set_charge_time: [0, 1440], set_close_charge_time: [0, 1440],
      set_gen_start_voltage: [40, 60], set_gen_stop_voltage: [40, 60],
      set_soc_back_utl: [0, 100], set_soc_back_bat: [0, 100],
      set_soc_back_gen: [0, 100], set_soc_close_gen: [0, 100],
      set_soc_cutoff: [0, 100], set_buzzer: [0, 1],
    };

    // 处理 42 键控制命令
    if (RANGES[action] != null) {
      const [min, max] = RANGES[action];
      let v = Number(value);
      if (value == null || Number.isNaN(v)) {
        result = 'INVALID_ARGS'; err = -2;
      } else {
        if (v < min || v > max) { result = 'INVALID_ARGS'; err = -2; }
        else {
          v = Math.round(v * 10) / 10; // 保留 0.1 精度
          this.ctrl[action] = v;
          applied = v;
          // 特殊联动
          if (action === 'set_soc_cutoff') this.battery.minSoc = v / 100;
        }
      }
      // 记日志并返回
      this._cmdLog.unshift({ ts: Date.now(), cmd: action, args: value != null ? value : null, result, applied_value: applied, revision: rev });
      if (this._cmdLog.length > 50) this._cmdLog.pop();
      return { result, applied_value: applied, revision: rev, err };
    }

    switch (action) {
      case 'power_on':
        this.ctrl.power_on = true; applied = 1;
        break;
      case 'power_off':
        this.ctrl.power_on = false; applied = 0;
        break;
      case 'query_telemetry':
      case 'query_config':
      case 'query_info':
        applied = null; result = 'QUERY'; // 上层据此触发补报
        break;
      default:
        result = 'NOT_SUPPORTED'; err = -4; applied = null;
    }

    // 记录命令日志
    this._cmdLog.unshift({
      ts: Date.now(),
      cmd: action,
      args: value != null ? value : null,
      result,
      applied_value: applied,
      revision: rev,
    });
    if (this._cmdLog.length > 50) this._cmdLog.pop();

    return { result, applied_value: applied, revision: rev, err };
  }

  /** 生成 config 上报体（协议 9.2 的 42 键 params + rev） */
  getConfigReport() {
    const params = {};
    const KEYS = [
      'set_output_priority','set_max_charge_current','set_ac_volt_range','set_battery_capacity',
      'set_battery_type','set_overload_restart','set_high_temp_restart','set_output_voltage',
      'set_output_frequency','set_master_slave','set_ac_charge_current','set_low_volt_return_utl',
      'set_high_volt_return_bat','set_charge_priority','set_alarm_control','set_backlight_ctrl',
      'set_power_shutdown_alarm','set_overload_use_city_power','set_max_discharge_current',
      'set_max_chg_curr','set_recover_threshold_volt','set_solar_power_balance','set_ac_output_mode',
      'set_li_bat_material','set_cell_serial_lifepo4','set_cell_serial_li_nmc','set_equalize_enable',
      'set_equalize_voltage','set_equalize_time','set_equalize_timeout','set_equalize_interval',
      'set_equalize_activate','set_charge_time','set_close_charge_time','set_gen_start_voltage',
      'set_gen_stop_voltage','set_soc_back_utl','set_soc_back_bat','set_soc_back_gen',
      'set_soc_close_gen','set_soc_cutoff','set_buzzer',
    ];
    for (const k of KEYS) if (this.ctrl[k] != null) params[k] = this.ctrl[k];
    return { rev: Math.floor(Date.now() / 1000), params };
  }

  /** 生成当前完整状态（供 detail 页 API 使用） */
  getStatus() {
    const snap = this.lastSnap || this._buildSnap();
    return {
      sn: this.dev.sn,
      ctrl: { ...this.ctrl },
      ctrlRevision: this._ctrlRevision,
      alarms: this._alarms.slice(),
      cmdLog: this._cmdLog.slice(0, 20),
      telemetry: snap && snap._meta || {},
    };
  }

  /** 生成告警（每步评估） */
  _evalAlarms(st) {
    const a = [];
    const soc = st.soc || 0;
    const batV = st.batVolt || 0;
    const tempC = st.tempC || 25;
    const pvW = st.pvW || 0;
    const gridV = (220 + (Math.random() * 6 - 3));
    const busV = st.busV || 400;

    if (soc < 0.05) a.push({ code: 4, name: '电池过放', level: 'error', ts: Date.now() });
    else if (soc < 0.10) a.push({ code: 5, name: '电池低电量', level: 'warn', ts: Date.now() });
    if (soc > 0.95) a.push({ code: 4, name: '电池过充', level: 'error', ts: Date.now() });
    if (busV > 480) a.push({ code: 2, name: 'BUS过压', level: 'error', ts: Date.now() });
    if (busV < 300) a.push({ code: 3, name: 'BUS欠压', level: 'warn', ts: Date.now() });
    if (tempC > 65) a.push({ code: 18, name: '温度过高', level: 'warn', ts: Date.now() });
    if (pvW > RATED_W * 1.1) a.push({ code: 0, name: 'PV过压', level: 'warn', ts: Date.now() });
    if (gridV > 253 || gridV < 187) a.push({ code: 14, name: '电网电压异常', level: 'warn', ts: Date.now() });

    // 随机模拟偶发故障（极低概率）
    if (Math.random() < 0.003) {
      const faults = [
        { code: 9, name: 'Inv故障', level: 'error' },
        { code: 10, name: 'BMS通信告警', level: 'warn' },
        { code: 22, name: 'EEPROM错误', level: 'warn' },
        { code: 23, name: '通信故障', level: 'warn' },
      ];
      const f = faults[Math.floor(Math.random() * faults.length)];
      a.push({ ...f, ts: Date.now() });
    }

    // 去重（相同 code 保留最新）
    const map = new Map();
    for (const x of [...this._alarms, ...a]) {
      if (!map.has(x.code) || map.get(x.code).ts < x.ts) map.set(x.code, x);
    }
    // 只保留最近 30 分钟内告警
    const cutoff = Date.now() - 30 * 60 * 1000;
    this._alarms = [...map.values()].filter(x => x.ts > cutoff);
    return this._alarms;
  }

  _buildSnap() {
    const now = new Date();
    const w = this.weather.sample(now, this._realWeather);
    return buildSnapshot({ pvW: 0, loadW: 0, batChargeW: 0, batDischargeW: 0, soc: 0.5, gridImportW: 0, gridExportW: 0, tempC: w.tempC, batVolt: 48.5, busV: 410, loadPercent: 20, curr: 0 }, this.eng, RATED_W);
  }

  async step() {
    const now = new Date();
    this.stepCount++;

    if (this.stepCount % 5 === 0) await this._refreshRealWeather();
    const w = this.weather.sample(now, this._realWeather);
    this.weather.step();

    const isWeekend = [0, 6].includes(now.getUTCDay());
    const loadW = this.load.step(now, isWeekend);

    // 关机时 PV=0，输出=0
    let pvW = 0;
    if (this.ctrl.power_on) {
      pvW = this.pv.step(w);
    }

    const bat = this.battery.step(pvW, loadW, 60);
    const invOut = this.ctrl.power_on ? Math.min(RATED_W, Math.max(pvW, loadW)) : 0;

    const st = {
      sn: this.dev.sn, tSec: Math.floor(now.getTime() / 1000),
      pvW, loadW,
      batChargeW: bat.chargeW,
      batDischargeW: bat.dischargeW,
      soc: bat.soc,
      gridImportW: bat.gridImportW,
      gridExportW: bat.gridExportW,
      tempC: w.tempC,
      batVolt: bat.voltageV,
      busV: bat.voltageV,
      loadPercent: Math.min(100, (loadW / RATED_W) * 100),
      curr: invOut / (this.ctrl.set_output_voltage || 230),
      powerOn: this.ctrl.power_on,
      // 扩展字段（直接来自模型）
      vPv1: pvW > 0 ? 350 + Math.random() * 30 : 0,
      vPv2: pvW > 0 ? 340 + Math.random() * 30 : 0,
      invOutW: invOut,
      invCurr: invOut / (this.ctrl.set_output_voltage || 230),
      boostTemp: 35 + (pvW / RATED_W) * 15 + Math.random() * 5,
      invTemp: 38 + (invOut / RATED_W) * 18 + Math.random() * 6,
      transformerTemp: 40 + (invOut / RATED_W) * 12 + Math.random() * 4,
      pvTemp: 30 + (pvW / RATED_W) * 20 + Math.random() * 4,
      mpptFan: Math.round(pvW > 50 ? 40 + (pvW / RATED_W) * 50 + Math.random() * 10 : 0),
      invFan: Math.round(invOut > 50 ? 30 + (invOut / RATED_W) * 60 + Math.random() * 10 : 0),
      // 交流侧（协议字段）
      acOutV: this.ctrl.set_output_voltage,
      acOutFreq: this.ctrl.set_output_frequency,
      acChargeW: this.ctrl.set_ac_charge_current * this.ctrl.set_output_voltage,
      acChgCurr: this.ctrl.set_ac_charge_current,
      gridVolt: 220.5 + (Math.random() * 4 - 2),
      gridFreq: 50 + (Math.random() * 0.2 - 0.1),
      // 诊断
      workTimeTotal: Math.floor(now.getTime() / 1000),
      faultCode: this._alarms.find(a => a.level === 'error' && a.code === 9) ? 0x01 : 0,
      warning: this._alarms.filter(a => a.level === 'error').reduce((v, a) => v | (1 << a.code), 0),
      bmsWarning: this._alarms.find(a => a.code === 10) ? 1 : 0,
      batOverCharge: bat.soc >= 0.99 ? 1 : 0,
    };

    const snap = buildSnapshot(st, this.eng, RATED_W);
    snap.sn = this.dev.sn;

    // --- 完整 _meta（30+ 字段，供电站详情页消费） ---
    const sysStatus = this._calcSysStatus(st);
    const wCode = this._weatherCode(w);
    snap._meta = {
      // --- 基本标识 ---
      sn: this.dev.sn,
      city: this._city ? this._city.name : '',
      // --- SysStatus ---
      SysStatus: sysStatus,
      // --- 光伏 ---
      Ppv: Math.round(st.pvW * 10),
      Vpv1: Math.round(st.vPv1 * 10),
      Vpv2: Math.round(st.vPv2 * 10),
      Buck1Curr: 0,
      Buck2Curr: 0,
      BoostTemp: Math.round(st.boostTemp * 10),
      // --- 逆变器输出 ---
      InvCurr: Math.round(st.invCurr * 10),
      OutputWatt: Math.round(st.invOutW * 10),
      OutputVA: Math.round(st.invOutW * 10),
      OutputCurr: Math.round(st.invCurr * 10),
      ACOutputVolt: Math.round((this.ctrl.set_output_voltage + (Math.random() * 4 - 2)) * 10),
      ACOutputFreq: Math.round((this.ctrl.set_output_frequency + (Math.random() * 0.4 - 0.2)) * 100),
      LoadPercent: Math.round(st.loadPercent * 10),
      // --- 逆变器温度 ---
      InvertTemp: Math.round(st.invTemp * 10),
      TransformerTemp: Math.round(st.transformerTemp * 10),
      // --- BUS ---
      BusVolt: Math.round(st.busV * 100),
      // --- 电池 ---
      BatterySOC: Math.round(st.soc * 100),
      BatVolt: Math.round(st.batVolt * 100),
      BatChgCurr: Math.round((bat.chargeW / bat.voltageV) * 10),
      BatDischgCurr: Math.round((bat.dischargeW / bat.voltageV) * 10),
      BatChrWatt: Math.round(bat.chargeW * 10),
      BatDisChrWatt: Math.round(bat.dischargeW * 10),
      BatOverCharge: 0,
      // --- 交流侧 ---
      GridVolt: Math.round((220 + Math.random() * 6 - 3) * 10),
      GridFreq: Math.round((50 + Math.random() * 0.4 - 0.2) * 100),
      ACChrWatt: Math.round(this.ctrl.set_ac_charge_current * this.ctrl.set_output_voltage * 10),
      ACChrCurr: Math.round(this.ctrl.set_ac_charge_current * 10),
      ACChrVA: Math.round(this.ctrl.set_ac_charge_current * this.ctrl.set_output_voltage * 10),
      ACInWatt: Math.round(bat.gridImportW * 10),
      ACInVA: Math.round(bat.gridImportW * 10),
      ACDisChrWatt: Math.round(bat.gridExportW * 10),
      ACDisChrVA: Math.round(bat.gridExportW * 10),
      // --- 风扇 ---
      MpptFanSpeed: st.mpptFan,
      InvFanSpeed: st.invFan,
      // --- 能量统计 ---
      EGen_today: Math.round(this.eng.EGen_today * 10),
      EGen_total: Math.round(this.eng.EGen_total * 10),
      Epv_today: Math.round(this.eng.Epv_today * 10),
      Epv_total: Math.round(this.eng.Epv_total * 10),
      Eac_chrToday: Math.round(this.eng.Eac_chrToday * 10),
      Eac_chrTotal: Math.round(this.eng.Eac_chrTotal * 10),
      Ebat_dischrToday: Math.round(this.eng.Ebat_dischrToday * 10),
      Ebat_dischrTotal: Math.round(this.eng.Ebat_dischrTotal * 10),
      Ebat_chrToday: Math.round(this.eng.Ebat_chrToday * 10),
      Ebat_chrTotal: Math.round(this.eng.Ebat_chrTotal * 10),
      Eac_dischrToday: Math.round(this.eng.Eac_dischrToday * 10),
      Eac_dischrTotal: 0,
      Eop_dischrToday: 0,
      Eop_dischrTotal: 0,
      WorkTimeTotal: Math.round(now.getTime() / 1000),
      // --- 告警 ---
      Warning: 0,
      BmsWarning: 0,
      FaultValue: 0,
      // --- 可调参数 ---
      maxChargeCurrent: this.ctrl.set_max_charge_current,
      acChargeCurrent: this.ctrl.set_ac_charge_current,
      outputVoltage: this.ctrl.set_output_voltage,
      outputFrequency: this.ctrl.set_output_frequency,
      socCutoff: this.ctrl.set_soc_cutoff,
      buzzerEn: this.ctrl.set_buzzer,
      powerOn: this.ctrl.power_on ? 1 : 0,
      // --- 天气 ---
      PvTemp: Math.round(st.boostTemp * 10),
      cloudCover: Math.round(w.cloudCover * 100),
      irradiance: Math.round(w.irradianceWm2),
      tempC: Math.round(w.tempC),
      humidity: Math.round(w.humidity || 0),
      isDay: !!w.isDay,
      sunrise: (this._realWeather && this._realWeather.sunrise) || null,
      sunset: (this._realWeather && this._realWeather.sunset) || null,
      weatherCode: wCode,
      weatherSrc: (this._realWeather && this._realWeather.source) || 'climate',
      _ctrlRevision: this._ctrlRevision,
    };

    // 更新告警
    this._evalAlarms(st);
    // 告警反映到 Warning 字段
    snap._meta.Warning = this._alarms.filter(a => a.level === 'error').reduce((v, a) => v | (1 << a.code), 0);
    snap._meta.BmsWarning = this._alarms.find(a => a.code === 10) ? 1 : 0;
    snap._meta.FaultValue = this._alarms.find(a => a.level === 'error' && a.code === 9) ? 0x01 : 0;

    // 能量累计
    const dtH = 1 / 60;
    const gen = (st.pvW * dtH) / 1000;
    const load_kwh = (st.loadW * dtH) / 1000;
    this.eng.EGen_today += gen;
    this.eng.Epv_today += gen;
    this.eng.loadToday += load_kwh;
    if (bat.chargeW > 0) {
      this.eng.Ebat_chrToday += (bat.chargeW * dtH) / 1000;
    }
    if (bat.dischargeW > 0) {
      this.eng.Ebat_dischrToday += (bat.dischargeW * dtH) / 1000;
    }

    // 快照存 store
    store.setTelemetry(this.dev.sn, st.tSec, snap._meta);
    this.lastSnap = snap;

    const nowS = st.tSec;
    if (nowS - this.lastHeartbeat >= HEARTBEAT_PERIOD_S) {
      this.lastHeartbeat = nowS;
      // heartbeat body: top-level sn/t去掉，信封由mqtt_publish统一包
      // data内包含 {sys,pv,ac,chr,bat,eng,fan,diag,sock, t:unix_sec}
      const hbBody = {
        t: nowS,
        sys: snap.sys, pv: snap.pv, ac: snap.ac,
        chr: snap.chr, bat: snap.bat, eng: snap.eng,
        fan: snap.fan, diag: snap.diag, sock: snap.sock,
      };
      if (this.onHeartbeat) this.onHeartbeat(this.dev.sn, hbBody);
    }
    return snap;
  }

  _calcSysStatus(st) {
    let b = 0;
    if (!this.ctrl.power_on) return 0x0001; // 关机=待机
    if (st.pvW > 20) b |= 0x0200;  // bit9 Pvinput
    if (st.loadW > 30) b |= 0x0100;  // bit8 ToLoad
    if (st.batChargeW > 5) b |= 0x0004;  // bit2 Charge
    if (st.batDischargeW > 5) b |= 0x0008;  // bit3 Discharge
    if (st.pvW > 0 && st.batChargeW > 0) b |= 0x0010;  // bit4 PvCharge
    if (this.ctrl.set_ac_charge_current > 0) b |= 0x0020;  // bit5 AcCharge
    if (st.gridImportW > 5) b |= 0x0400;  // bit10 AcInput
    if (b === 0) b = 0x0001;
    return b;
  }

  _weatherCode(w) {
    if (!w.isDay) return 0; // night clear
    const c = w.cloudCover;
    if (c < 0.15) return 1;  // sunny
    if (c < 0.5) return 2;   // partly cloudy
    if (c < 0.8) return 3;   // cloudy
    return 4;                // overcast
  }

  stop() { /* noop */ }
}

/** 管理所有被认领设备的模拟实例 */
const instances = new Map();
let sharedProvider = null;

function ensureProvider() {
  if (!sharedProvider) {
    const disable = String(process.env.WEATHER_DISABLE || '0') === '1';
    sharedProvider = disable ? null : new WeatherProvider();
  }
  return sharedProvider;
}

function ensureDevice(dev) {
  if (instances.has(dev.sn)) return instances.get(dev.sn);
  const sim = new DeviceSim(dev, ensureProvider());
  instances.set(dev.sn, sim);
  return sim;
}

async function stepAll(onHeartbeat) {
  const devs = store.claimedDevices();
  await Promise.allSettled(devs.map(async (dev) => {
    const sim = ensureDevice(dev);
    sim.onHeartbeat = onHeartbeat;
    try {
      await sim.step();
    } catch (e) {
      console.error(`[sim] ${dev.sn} step error:`, e && e.message);
    }
  }));
}

function removeDevice(sn) { return instances.delete(sn); }
function getSnapshot(sn) { const s = instances.get(sn); return s ? s.lastSnap : null; }
function getSim(sn) { return instances.get(sn) || null; }
function count() { return instances.size; }

function start(onHeartbeat) {
  stepAll(onHeartbeat);
  setInterval(() => stepAll(onHeartbeat), STEP_MS);
  console.log(`[sim] engine started, tick=${STEP_MS}ms`);
}

module.exports = { start, stepAll, ensureDevice, ensureDevice, removeDevice, getSnapshot, getSim, count, DeviceSim };

