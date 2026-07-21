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

test('ESP32-C2 keeps the App AP as the default transport and checks Wi-Fi failures', () => {
  const app = read('esp32c2_idf/main/app_main.c');
  const wifiMode = app.indexOf('esp_wifi_set_mode(WIFI_MODE_APSTA)');
  const wifiStart = app.indexOf('esp_wifi_start()');
  const apiStart = app.indexOf('ap_api_server_start()');
  assert.ok(wifiMode >= 0 && wifiStart > wifiMode && apiStart > wifiStart,
    'APSTA and the App API must be available during normal startup');
  assert.doesNotMatch(app, /ble_prov_start\s*\(/,
    'BLE must not exhaust the heap while the App AP and HTTP server are active');
  assert.match(app, /ret\s*=\s*esp_wifi_set_mode\(WIFI_MODE_APSTA\)[\s\S]*ret\s*!=\s*ESP_OK/);
  assert.match(app, /ret\s*=\s*esp_wifi_start\(\)[\s\S]*ret\s*!=\s*ESP_OK/);
});

test('short button press switches from App AP to the BLE provisioning transport', () => {
  const state = read('esp32c2_idf/main/state_machine.c');
  const button = state.slice(state.indexOf('void check_button(void)'));
  const stopApi = button.indexOf('ap_api_server_stop()');
  const staOnly = button.indexOf('esp_wifi_set_mode(WIFI_MODE_STA)');
  const startBle = button.indexOf('ble_prov_start(g_device_sn, FIRMWARE_VERSION)');
  assert.ok(stopApi >= 0 && staOnly > stopApi && startBle > staOnly,
    'BLE provisioning must start only after AP/HTTP memory is released');
});

test('BLE provisioning releases its stack and excludes unused NimBLE features', () => {
  const ble = read('esp32c2_idf/components/ble_prov/ble_prov.c');
  assert.match(ble, /case\s+WIFI_PROV_END[\s\S]*wifi_prov_mgr_deinit\s*\(\)/);
  assert.match(ble, /void ble_prov_stop\s*\([^)]*\)[\s\S]*wifi_prov_mgr_stop_provisioning\s*\(\)/);
  assert.doesNotMatch(ble, /retrying in 2s|pdMS_TO_TICKS\(2000\)/,
    'BTDM memory release is one-way, so a failed BLE start must return to AP instead of retrying');
  const success = ble.indexOf('case WIFI_PROV_CRED_SUCCESS');
  const ended = ble.indexOf('case WIFI_PROV_END');
  const callback = ble.indexOf('s_callback(', ended);
  assert.ok(success >= 0 && ended > success && callback > ended,
    'Wi-Fi callback must run after BLE manager deinit has released memory');

  const defaults = read('esp32c2_idf/sdkconfig.defaults');
  for (const symbol of [
    'PROX_SERVICE', 'ANS_SERVICE', 'CTS_SERVICE', 'HTP_SERVICE', 'IPSS_SERVICE',
    'TPS_SERVICE', 'IAS_SERVICE', 'LLS_SERVICE', 'SPS_SERVICE', 'HR_SERVICE',
    'BAS_SERVICE', 'DIS_SERVICE', 'DTM_MODE_TEST', 'SM_SIGN_CNT', 'CPFD_CAFD',
    'RECONFIG_MTU', 'EXTRA_ADV_FIELDS',
  ]) {
    assert.match(defaults, new RegExp(`# CONFIG_BT_NIMBLE_${symbol} is not set`),
      `unused NimBLE feature ${symbol} must be disabled`);
  }
});
