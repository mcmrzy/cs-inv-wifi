const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '..', '..');
const read = (relativePath) => fs.readFileSync(path.join(root, relativePath), 'utf8');

const FRAME_HEADER = 0xaa;
const FRAME_ESCAPE = 0x55;
const ESCAPED_HEADER = 0x01;
const ESCAPED_ESCAPE = 0x00;

function xorChecksum(cmd, data) {
  let checksum = FRAME_HEADER ^ cmd ^ ((data.length >> 8) & 0xff) ^ (data.length & 0xff);
  for (const byte of data) checksum ^= byte;
  return checksum;
}

function escapeByte(byte, output) {
  if (byte === FRAME_HEADER) output.push(FRAME_ESCAPE, ESCAPED_HEADER);
  else if (byte === FRAME_ESCAPE) output.push(FRAME_ESCAPE, ESCAPED_ESCAPE);
  else output.push(byte);
}

function encodeFrame(cmd, data) {
  const output = [FRAME_HEADER];
  for (const byte of [cmd, data.length >> 8, data.length & 0xff, ...data, xorChecksum(cmd, data)]) {
    escapeByte(byte, output);
  }
  return Buffer.from(output);
}

function decodeFrame(frame) {
  assert.equal(frame[0], FRAME_HEADER, 'frame must start with 0xAA');
  const decoded = [];
  for (let i = 1; i < frame.length; i += 1) {
    const byte = frame[i];
    if (byte !== FRAME_ESCAPE) {
      decoded.push(byte);
      continue;
    }
    assert.ok(i + 1 < frame.length, 'dangling escape byte');
    const escaped = frame[++i];
    if (escaped === ESCAPED_HEADER) decoded.push(FRAME_HEADER);
    else if (escaped === ESCAPED_ESCAPE) decoded.push(FRAME_ESCAPE);
    else assert.fail(`invalid escape sequence 0x55 0x${escaped.toString(16)}`);
  }

  assert.ok(decoded.length >= 4, 'frame is too short');
  const cmd = decoded[0];
  const length = (decoded[1] << 8) | decoded[2];
  assert.equal(decoded.length, length + 4, 'declared length must match the frame');
  const data = decoded.slice(3, 3 + length);
  assert.equal(decoded.at(-1), xorChecksum(cmd, data), 'XOR checksum mismatch');
  return { cmd, data: Buffer.from(data) };
}

function parseSize(value) {
  const normalized = value.trim().toLowerCase();
  if (normalized.endsWith('k')) return Number.parseInt(normalized, 10) * 1024;
  if (normalized.endsWith('m')) return Number.parseInt(normalized, 10) * 1024 * 1024;
  return Number.parseInt(normalized, normalized.startsWith('0x') ? 16 : 10);
}

function parsePartitions(csv) {
  return csv.split(/\r?\n/)
    .map((line) => line.replace(/#.*/, '').trim())
    .filter(Boolean)
    .map((line) => {
      const [name, type, subtype, offset, size] = line.split(',').map((part) => part.trim());
      return { name, type, subtype, offset: parseSize(offset), size: parseSize(size) };
    });
}

function functionBody(source, signature, nextSignature) {
  const start = source.indexOf(signature);
  assert.notEqual(start, -1, `missing function: ${signature}`);
  const end = source.indexOf(nextSignature, start + signature.length);
  assert.notEqual(end, -1, `missing boundary function: ${nextSignature}`);
  return source.slice(start, end);
}

test('UART reference codec round-trips header, escape, length and checksum bytes', () => {
  const vectors = [
    { cmd: 0x20, data: [] },
    { cmd: FRAME_HEADER, data: [0x00, FRAME_HEADER, FRAME_ESCAPE, 0xff] },
    { cmd: FRAME_ESCAPE, data: Array.from({ length: 0xaa }, (_, i) => i & 0xff) },
  ];

  for (const vector of vectors) {
    const encoded = encodeFrame(vector.cmd, vector.data);
    const decoded = decodeFrame(encoded);
    assert.equal(decoded.cmd, vector.cmd);
    assert.deepEqual([...decoded.data], vector.data);
    assert.equal(encoded.subarray(1).includes(FRAME_HEADER), false,
      'an escaped frame must not contain a raw 0xAA after its header');
  }
});

for (const [name, file] of [
  ['Arduino', 'src/uart_protocol.cpp'],
  ['ESP-IDF', 'esp32c2_idf/main/uart_protocol.c'],
]) {
  test(`${name} UART implementation escapes and restores both reserved bytes`, () => {
    const source = read(file);
    assert.match(source, /b\s*==\s*FRAME_HEADER[\s\S]{0,250}FRAME_ESCAPE[\s\S]{0,100}FRAME_ESC_HEADER/);
    assert.match(source, /b\s*==\s*FRAME_ESCAPE[\s\S]{0,250}FRAME_ESCAPE[\s\S]{0,100}FRAME_ESC_ESCAPE/);
    assert.match(source, /g_rx_esc_next[\s\S]{0,500}FRAME_ESC_HEADER[\s\S]{0,150}FRAME_HEADER/);
    assert.match(source, /FRAME_ESC_ESCAPE[\s\S]{0,150}FRAME_ESCAPE/);
  });
}

test('ESP-IDF custom partition table fits a 4 MiB flash without overlap', () => {
  const partitions = parsePartitions(read('esp32c2_idf/partitions.csv'));
  const flashSize = 4 * 1024 * 1024;
  assert.deepEqual(partitions.map((part) => part.name),
    ['nvs', 'phy_init', 'otadata', 'ota_0', 'ota_1', 'cache']);

  for (let i = 0; i < partitions.length; i += 1) {
    const part = partitions[i];
    assert.ok(part.size > 0, `${part.name} must not be empty`);
    assert.ok(part.offset + part.size <= flashSize, `${part.name} exceeds 4 MiB flash`);
    if (i > 0) {
      const previous = partitions[i - 1];
      assert.ok(part.offset >= previous.offset + previous.size,
        `${part.name} overlaps ${previous.name}`);
    }
  }

  const otaSlots = partitions.filter((part) => part.type === 'app' && part.subtype.startsWith('ota_'));
  assert.equal(otaSlots.length, 2, 'two OTA slots are required for rollback');
  assert.equal(otaSlots[0].size, otaSlots[1].size, 'OTA slots must be the same size');
  assert.ok(otaSlots[0].size >= 0x1c0000, 'each OTA slot must hold the current firmware');
  for (const slot of otaSlots) {
    assert.equal(slot.offset % 0x10000, 0, `${slot.name} must use 64 KiB app alignment`);
  }
  assert.equal(partitions.at(-1).offset + partitions.at(-1).size, flashSize,
    'the final partition must end at the 4 MiB boundary');
  assert.ok(partitions.at(-1).size >= 0x60000,
    'cache must stage a 256 KiB ARM image while retaining filesystem overhead');

  const sdkconfig = read('esp32c2_idf/sdkconfig.defaults');
  assert.match(sdkconfig, /CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y/);
  assert.match(sdkconfig, /CONFIG_PARTITION_TABLE_CUSTOM=y/);
  assert.match(sdkconfig, /CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions\.csv"/);
});

test('Arduino custom partition table provides aligned dual OTA slots on 4 MiB flash', () => {
  const partitions = parsePartitions(read('partitions_arduino_4mb.csv'));
  const flashSize = 4 * 1024 * 1024;
  assert.deepEqual(partitions.map((part) => part.name),
    ['nvs', 'otadata', 'app0', 'app1', 'spiffs', 'coredump']);

  for (let i = 0; i < partitions.length; i += 1) {
    const part = partitions[i];
    assert.ok(part.size > 0, `${part.name} must not be empty`);
    assert.ok(part.offset + part.size <= flashSize, `${part.name} exceeds 4 MiB flash`);
    if (i > 0) {
      const previous = partitions[i - 1];
      assert.ok(part.offset >= previous.offset + previous.size,
        `${part.name} overlaps ${previous.name}`);
    }
  }

  const otaSlots = partitions.filter((part) => part.type === 'app' && part.subtype.startsWith('ota_'));
  assert.equal(otaSlots.length, 2, 'two OTA slots are required for rollback');
  assert.equal(otaSlots[0].size, otaSlots[1].size, 'OTA slots must be the same size');
  assert.ok(otaSlots[0].size >= 0x1a0000, 'each OTA slot must hold the signed firmware');
  for (const slot of otaSlots) {
    assert.equal(slot.offset % 0x10000, 0, `${slot.name} must use 64 KiB app alignment`);
  }
  assert.equal(partitions.at(-1).offset + partitions.at(-1).size, flashSize,
    'the final partition must end at the 4 MiB boundary');

  const platformio = read('platformio.ini');
  assert.match(platformio, /^board_build\.partitions\s*=\s*partitions_arduino_4mb\.csv\s*$/m);
  assert.equal((platformio.match(/^board_build\.partitions\s*=/gm) || []).length, 1,
    'PlatformIO must select exactly one partition table');
});

test('Arduino command parser requires numeric t and suppresses duplicate task_id', () => {
  const source = read('src/main.cpp');
  assert.match(source, /deserializeJson\s*\([^;]+payload[^;]+len/);
  assert.match(source, /doc\["t"\]\.is<[^>]+>\(\)/);
  assert.match(source, /(?:task_seen|command_seen|find_task)\s*\(\s*task_id\s*\)/);
  assert.match(source, /(?:remember_task|remember_command)\s*\(\s*task_id\b/);
  const duplicateCheck = Math.max(
    source.indexOf('task_seen(task_id)'),
    source.indexOf('command_seen(task_id)'),
    source.indexOf('find_task(task_id)'),
  );
  assert.ok(duplicateCheck < source.indexOf('uart_send_frame(CMD_COMMAND_RECV'),
    'duplicate detection must happen before forwarding to ARM');
});

test('ESP-IDF command parser requires numeric t and suppresses duplicate task_id', () => {
  const source = read('esp32c2_idf/main/state_machine.c');
  assert.doesNotMatch(source, /extract_str\s*\(\s*payload\s*,\s*"\\"t\\""/,
    'numeric JSON t must not be read with a string extractor');
  assert.match(source, /cJSON_(?:ParseWithLength|ParseWithLengthOpts)\s*\(/);
  assert.match(source, /cJSON_GetObjectItem[^;]+"t"/);
  assert.match(source, /cJSON_IsNumber\s*\(/);
  assert.match(source, /(?:task_seen|command_seen)\s*\(\s*task_id\s*\)/);
  assert.match(source, /(?:remember_task|remember_command)\s*\(\s*task_id\b/);
  const duplicateCheck = Math.max(source.indexOf('task_seen(task_id)'), source.indexOf('command_seen(task_id)'));
  assert.ok(duplicateCheck < source.indexOf('uart_send_frame(CMD_COMMAND_RECV'),
    'duplicate detection must happen before forwarding to ARM');
});

for (const [name, file, statusSignature, errorSignature] of [
  ['Arduino', 'src/mqtt_ota_handler.cpp', 'void mqtt_ota_report_status(', 'void mqtt_ota_report_error('],
  ['ESP-IDF', 'esp32c2_idf/main/mqtt_ota_adapter.c', 'void mqtt_ota_report_status(', 'void mqtt_ota_report_error('],
]) {
  test(`${name} OTA status includes the complete correlation and progress schema`, () => {
    const body = functionBody(read(file), statusSignature, errorSignature);
    for (const field of ['task_id', 'target', 'version', 'state', 'progress', 'code', 'message']) {
      assert.match(body, new RegExp(`"${field}"`), `${name} OTA status is missing ${field}`);
    }
    assert.match(body, /PROTO_TOPIC_OTA_STATUS/);
  });

  test(`${name} OTA error status preserves task correlation`, () => {
    const source = read(file);
    const body = functionBody(source, errorSignature, 'static void send_ota_ack(');
    for (const field of ['task_id', 'target', 'version', 'state', 'progress', 'code', 'message']) {
      assert.match(body, new RegExp(`"${field}"`), `${name} OTA error status is missing ${field}`);
    }
    assert.match(body, /PROTO_OTA_STATE_FAILED/);
    assert.match(body, /PROTO_TOPIC_OTA_STATUS/);
  });
}

test('Arduino and ESP-IDF expose the same OTA state vocabulary', () => {
  const states = (source) => new Map([...source.matchAll(
    /#define\s+(PROTO_OTA_STATE_[A-Z_]+)\s+"([a-z_]+)"/g,
  )].map((match) => [match[1], match[2]]));

  assert.deepEqual(
    states(read('src/cs_protocol_defs.h')),
    states(read('esp32c2_idf/main/cs_protocol_defs.h')),
  );
});
