import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
const absolute = (relativePath) => resolve(root, relativePath);
const read = (relativePath) => readFileSync(absolute(relativePath), 'utf8');

test('ESP32-C2 exposes one OTA service for remote MQTT and local App uploads', () => {
  assert.equal(existsSync(absolute('esp32c2_idf/main/ota_service.h')), true);
  assert.equal(existsSync(absolute('esp32c2_idf/main/ota_service.c')), true);

  const header = read('esp32c2_idf/main/ota_service.h');
  for (const symbol of [
    'OTA_SOURCE_MQTT',
    'OTA_SOURCE_LOCAL',
    'OTA_TARGET_ESP',
    'OTA_TARGET_ARM',
    'OtaManifest',
    'OtaSnapshot',
    'ota_service_start_remote',
    'ota_service_start_local',
    'ota_service_cancel',
    'ota_service_get_snapshot',
  ]) {
    assert.match(header, new RegExp(`\\b${symbol}\\b`), `missing ${symbol}`);
  }

  const mqtt = read('esp32c2_idf/main/mqtt_ota_adapter.c');
  assert.match(mqtt, /#include\s+"ota_service\.h"/);
  assert.match(mqtt, /ota_service_start_remote\s*\(/);

  const local = read('esp32c2_idf/main/ap_api_server.c');
  assert.match(local, /#include\s+"ota_service\.h"/);
  assert.match(local, /ota_service_start_local\s*\(/);
  assert.match(local, /"X-OTA-Size"/);
  assert.match(local, /declared_size\s*==\s*manifest->file_size/);
});

test('AP server keeps App provisioning and OTA APIs without browser pages', () => {
  const source = read('esp32c2_idf/main/ap_api_server.c');
  for (const route of [
    '/scan',
    '/config',
    '/wifi_status',
    '/ota/info',
    '/ota/progress',
    '/ota/upload',
    '/ota/cancel',
  ]) {
    assert.ok(source.includes(route), `missing App API route ${route}`);
  }
  assert.doesNotMatch(source, /<!DOCTYPE|<html|<style|<script/i);
  assert.doesNotMatch(source, /\.uri\s*=\s*"\/(?:index\.html|status|ota)"/);
  assert.match(source, /httpd_start\s*\(/);
});

test('Wi-Fi and IP events are separated by event base', () => {
  const source = read('esp32c2_idf/main/app_main.c');
  assert.match(source, /event_base\s*==\s*WIFI_EVENT/);
  assert.match(source, /event_base\s*==\s*IP_EVENT/);

  const wifiBranch = source.indexOf('event_base == WIFI_EVENT');
  const ipBranch = source.indexOf('event_base == IP_EVENT');
  const gotIpCast = source.indexOf('ip_event_got_ip_t');
  assert.ok(wifiBranch >= 0 && ipBranch >= 0 && gotIpCast > ipBranch,
    'IP event data must only be interpreted inside the IP_EVENT branch');
});

test('main loop and state machine do not poll a blocking socket server', () => {
  const app = read('esp32c2_idf/main/app_main.c');
  const state = read('esp32c2_idf/main/state_machine.c');
  assert.doesNotMatch(app, /tcp_provision_process\s*\(/);
  assert.doesNotMatch(state, /tcp_provision_process\s*\(/);
});

test('offline cache batches writes and periodically persists partial batches', () => {
  const cache = read('esp32c2_idf/main/offline_cache.c');
  const addStart = cache.indexOf('bool offline_cache_add(');
  const processStart = cache.indexOf('void offline_cache_process(');
  assert.ok(addStart >= 0 && processStart > addStart);
  const addBody = cache.slice(addStart, processStart);
  assert.match(addBody, /s_batch_count\s*>=\s*MAX_BATCH_ITEMS/);
  assert.doesNotMatch(addBody, /bool stored\s*=\s*flush_batch\(\)\s*==\s*ESP_OK/,
    'every individual message must not force a flash write');

  const header = read('esp32c2_idf/main/offline_cache.h');
  assert.match(header, /void offline_cache_process\s*\(void\)/);
  const app = read('esp32c2_idf/main/app_main.c');
  assert.match(app, /offline_cache_process\s*\(\)/);
});

test('offline QoS 1 records are removed only after MQTT PUBACK', () => {
  const mqtt = read('esp32c2_idf/main/mqtt_handler.c');
  assert.match(mqtt, /MQTT_EVENT_PUBLISHED/);
  assert.match(mqtt, /xQueueReceive\s*\(\s*g_offline_ack_queue/);
  assert.match(mqtt, /offline_cache_flush_task/);

  const callbackStart = mqtt.indexOf('static bool offline_send_callback(');
  const callbackEnd = mqtt.indexOf('static void mqtt_event_handler(', callbackStart);
  const callback = mqtt.slice(callbackStart, callbackEnd);
  assert.match(callback, /esp_mqtt_client_publish[\s\S]*xQueueReceive/,
    'offline send must wait for PUBACK after queueing a QoS 1 publish');
});

test('ESP rollback and per-target security versions are configured', () => {
  const defaults = read('esp32c2_idf/sdkconfig.defaults');
  assert.match(defaults, /CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y/);

  const storage = read('esp32c2_idf/main/nvs_storage.c');
  assert.match(storage, /ota_sec_esp/);
  assert.match(storage, /ota_sec_arm/);
  assert.match(storage, /ota_pending/);

  const service = read('esp32c2_idf/main/ota_service.c');
  assert.match(service, /ESP_OTA_IMG_PENDING_VERIFY/);
  assert.match(service, /esp_ota_mark_app_valid_cancel_rollback\s*\(/);
  assert.match(service, /ota_service_note_main_loop_alive\s*\(/);
});
