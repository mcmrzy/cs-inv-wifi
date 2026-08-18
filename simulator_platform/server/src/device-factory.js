'use strict';
/*
 * device-factory.js — SN 生成 + PIN 计算 + 二维码载荷
 *
 * SN 格式对齐 tools/sn_encoder.py (CS-SN-STD-001 V1.1):
 *   16位: MM-CC-PPPP-YY-SSSSS-C
 *   MM     = 厂商码 (2位, H1=总部1号工厂)
 *   CC     = 国家码 (2位, ISO 3166-1 alpha-2, NC=内测)
 *   PPPP   = 客户码 (4位: grade 1位 + 客户流水 3位)
 *   YY     = 日期码 (2位: 年份码 1位 + 月份码 1位)
 *   SSSSS  = 生产序列号 (5位)
 *   C      = 校验位 (CRC-16 MODBUS mod 36)
 *
 * 校验位算法 (与固件 sn_encoder.py 完全一致):
 *   1. 对前15位 ASCII 字节做 CRC-16 MODBUS (Poly=0x8005, Init=0xFFFF, RefIn=true, RefOut=true)
 *   2. 取低8位 (crc & 0xFF)
 *   3. 对36取模 (lo % 36)
 *   4. 映射为36进制字符 (0-9→'0'-'9', 10-35→'A'-'Z')
 *
 * PIN 算法对齐 tools/pin_test_vectors.py (与固件 ble_ct_compute_pin() 一致):
 *   PRODUCT_SECRET = "CS_INV_L10_2026_SECRET"
 *   PIN = HMAC-SHA256(PRODUCT_SECRET, sn)[0]<<16 | [1]<<8 | [2] % 1000000
 *
 * 二维码内容 (QR Payload):
 *   JSON: { sn, pin, model, rated_kw, country, date, seq }
 */
const crypto = require('crypto');

const PRODUCT_SECRET = 'CS_INV_L10_2026_SECRET';

// 默认生产参数（内测 CN；NC 为内测自造码，不在手机 App ISO 3166 白名单中，扫描会报 SN 格式错误）
const DEFAULTS = {
  manufacturer:  'H1',   // 厂商码 H1=总部1号工厂
  country:       'CN',   // CN=中国（App 白名单内）
  grade:         'A',    // 客户等级 A=战略大客户
  customerStart: 1,     // 客户流水起始
  year:          2025,  // 生产年份
  month:         10,    // 生产月份
};

// ==================== CRC-16 MODBUS ====================
const _CRC16_TABLE = (() => {
  const t = new Uint16Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let j = 0; j < 8; j++) {
      c = (c & 1) ? 0xA001 ^ (c >>> 1) : (c >>> 1);
    }
    t[i] = c;
  }
  return t;
})();

function crc16Modbus(data) {
  let crc = 0xFFFF;
  for (let i = 0; i < data.length; i++) {
    crc = _CRC16_TABLE[(crc ^ data[i]) & 0xFF] ^ (crc >>> 8);
  }
  return crc;
}

// ==================== SN 核心 ====================
// 年份码（33进制，跳过 O 和 I）
const YEAR_CODES = '123456789ABCDEFGHJKLMNPQRSTUVWXYZ';
const YEAR_BASE = 2024;

// 月份码
const MONTH_CODES = { 1:'1',2:'2',3:'3',4:'4',5:'5',6:'6',7:'7',8:'8',9:'9',10:'A',11:'B',12:'C' };
const MONTH_DECODE = Object.fromEntries(Object.entries(MONTH_CODES).map(([k,v])=>[v,parseInt(k)]));
const VALID_GRADES = ['A','B','C','D','E','P','X'];

function yearEncode(year) {
  const offset = Math.max(0, year - YEAR_BASE) % YEAR_CODES.length;
  return YEAR_CODES[offset];
}
function yearDecode(code) {
  const i = YEAR_CODES.indexOf(code.toUpperCase());
  return i >= 0 ? YEAR_BASE + i : -1;
}
function monthEncode(month) { return MONTH_CODES[month] || '?'; }

// 校验位计算
function _valChar(v) {
  return v < 10 ? String.fromCharCode(0x30 + v) : String.fromCharCode(0x41 + v - 10);
}

function computeCheckDigit(prefix15) {
  // prefix15: 15-char string
  const buf = Buffer.from(prefix15, 'ascii');
  const crc = crc16Modbus(buf);
  return _valChar((crc & 0xFF) % 36);
}

// 生成单条 SN
function _generateOne(cfg) {
  const mfr      = (cfg.manufacturer  || DEFAULTS.manufacturer).toUpperCase().slice(0,2).padEnd(2,'?');
  const country  = (cfg.country        || DEFAULTS.country).toUpperCase().slice(0,2).padEnd(2,'?');
  const grade    = (cfg.grade          || DEFAULTS.grade).toUpperCase();
  const custNum = Number(cfg.customerStart || DEFAULTS.customerStart);
  const yearCode = yearEncode(cfg.year    || DEFAULTS.year);
  const monthCode = monthEncode(cfg.month  || DEFAULTS.month);
  const seqNum  = Number(cfg.seqStart || 1);

  const custStr = grade + String(custNum).padStart(3,'0');
  const seqStr  = String(seqNum).padStart(5,'0');
  const dateCode = yearCode + monthCode;

  const prefix = mfr + country + custStr + dateCode + seqStr;
  if (prefix.length !== 15) return null;
  const check  = computeCheckDigit(prefix);
  return prefix + check;
}

// 生成批次（自动递增流水号/序列号保证唯一）
let _seqCounter   = 1;
let _custCounter  = DEFAULTS.customerStart;

function nextSN(cfg = {}) {
  // 支持批量：自动递增
  const seq   = cfg.seqStart != null ? cfg.seqStart + _seqCounter - 1 : _seqCounter;
  const cust  = cfg.customerStart != null ? cfg.customerStart + _custCounter - 1 : _custCounter;

  const sn = _generateOne({
    ...DEFAULTS,
    ...cfg,
    seqStart:    seq,
    customerStart: cust,
  });

  _seqCounter++;
  if (_seqCounter > 99999) { _seqCounter = 1; _custCounter++; }
  return sn;
}

// 校验 SN
function validateSN(sn) {
  if (!sn || typeof sn !== 'string' || sn.length !== 16) return false;
  sn = sn.toUpperCase();
  if (!/^[A-Z0-9]+$/.test(sn)) return false;
  if (!/^[A-Z]$/.test(sn[0])) return false; // 厂商码第1位字母
  if (!VALID_GRADES.includes(sn[4]) && !/^[0-9]$/.test(sn[4])) return false;
  if (!sn.slice(5,8).match(/^\d{3}$/)) return false;
  if (!YEAR_CODES.includes(sn[8])) return false;
  if (!MONTH_DECODE[sn[9]]) return false;
  if (!sn.slice(10,15).match(/^\d{5}$/)) return false;
  const expected = computeCheckDigit(sn.slice(0,15));
  return expected === sn[15];
}

// 解析 SN
function parseSN(sn) {
  if (!sn || typeof sn !== 'string' || sn.length !== 16) return null;
  sn = sn.toUpperCase();
  if (!validateSN(sn)) return null;
  return {
    manufacturer:  sn.slice(0,2),
    country:       sn.slice(2,4),
    customer:      sn.slice(4,8),
    grade:         sn[4],
    custNum:       parseInt(sn.slice(5,8), 10),
    dateCode:      sn.slice(8,10),
    yearCode:      sn[8],
    year:          yearDecode(sn[8]),
    monthCode:     sn[9],
    month:         MONTH_DECODE[sn[9]] || 0,
    sequence:      sn.slice(10,15),
    seqNum:        parseInt(sn.slice(10,15), 10),
    checkDigit:    sn[15],
  };
}

// 序列号唯一性：基于全局计数器，每次 restart 重新计数
let _globalSeq = 1;
function randomSN(cfg = {}) {
  const sn = _generateOne({
    ...DEFAULTS,
    ...cfg,
    seqStart:    _globalSeq++,
  });
  return sn || 'H1NCA0012500001' + computeCheckDigit('H1NCA0012500001');
}

// ==================== PIN ====================
function computePin(sn) {
  const secret = PRODUCT_SECRET;
  const digest = crypto
    .createHmac('sha256', secret)
    .update(String(sn), 'utf8')
    .digest();
  const val = (digest[0] << 16) | (digest[1] << 8) | digest[2];
  return String(val % 1000000).padStart(6, '0');
}

// ==================== 二维码 Payload ====================
// App (sn_utils.dart parseQRCode) 支持格式:
//   "SN:<sn> PIN:<pin>" → 自动提取 SN+PIN，直接绑定免弹窗
//   "https://xxx/bind?sn=..&pin=.." → 自动提取
//   纯 "SN:<sn>" → 无 PIN，App 弹窗要求手动输入
// 对齐 tools/sn_batch_gui.py 风格并追加 PIN，保证 App 免弹窗绑定
function qrcodePayload(dev) {
  return `SN:${dev.sn} PIN:${dev.pin}`;
}

// ==================== 工厂函数 ====================
function createDevice(cfg = {}) {
  const sn = (cfg.sn) || randomSN(cfg);
  const pin = computePin(sn);
  const parsed = parseSN(sn);
  const city = cfg.city_code ? require('./geo').getCity(cfg.city_code) : null;
  const dev = {
    sn,
    pin,
    model:     'CS-L10-6K2',
    rated_w:   6200,
    rated_kw:  6.2,
    city_code: city ? city.code : (cfg.city_code || null),
    city_name: city ? city.name : (cfg.city_name || ''),
    province:  city ? city.province : (cfg.province  || ''),
    country:   parsed ? parsed.country : DEFAULTS.country,
    grade:     parsed ? parsed.grade  : DEFAULTS.grade,
    mfg_date:  parsed ? `${parsed.year}-${String(parsed.month).padStart(2,'0')}` : null,
    claimed:   false,
    claimed_by: null,
    claimed_at: null,
  };
  dev.qrcode = qrcodePayload(dev);
  return dev;
}

module.exports = {
  randomSN,
  computePin,
  qrcodePayload,
  createDevice,
  parseSN,
  validateSN,
  computeCheckDigit,
  DEFAULTS,
  PRODUCT_SECRET,
};
