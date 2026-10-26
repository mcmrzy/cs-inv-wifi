#include "ble_ct_cmd.h"
#include "ble_ct.h"
#include "ble_ct_clock.h"
#include "ble_ct_frames.h"
#include "ble_prov.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "freertos/FreeRTOS.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#define TOKEN_MAX_LEN 64
#define RESULT_CAPACITY 1024u
#define COMMAND_CAPACITY 1024u
#define CACHE_BODY_CAPACITY 512u
#define COMMAND_DIGEST_SIZE 32u
#define CACHE_SIZE 8u
#define CACHE_EXPIRE_MS (10u * 60u * 1000u)
#define RECEIVE_TIMEOUT_MS 5000u
#define WRITE_TIMEOUT_MS 30000u
#define QUEUE_SIZE 4u

typedef struct {
    char json[COMMAND_CAPACITY + 1u];
    uint32_t epoch;
} QueuedCommand;
typedef struct {
    bool valid;
    char command_id[TOKEN_MAX_LEN + 1u];
    uint8_t canonical_digest[COMMAND_DIGEST_SIZE];
    char body[CACHE_BODY_CAPACITY + 1u];
    uint32_t ts_ms;
} CmdCache;
typedef struct {
    bool v2;
    uint32_t epoch;
    char session[TOKEN_MAX_LEN + 1u];
    char message[TOKEN_MAX_LEN + 1u];
    char operation[TOKEN_MAX_LEN + 1u];
    char command[TOKEN_MAX_LEN + 1u];
} Request;
typedef struct {
    bool active;
    uint32_t token;
    uint32_t started_ms;
    Request request;
    char param_id[TOKEN_MAX_LEN + 1u];
} PendingWrite;

/* GATT only copies bounded bytes. JSON, cache and executor callbacks all run
 * in the main loop. The short critical section never includes allocation,
 * notification, NVS or UART work. Connection reset preserves accepted work. */
static portMUX_TYPE s_rx_lock = portMUX_INITIALIZER_UNLOCKED;
static ble_ct_frame_rx_t s_rx;
static QueuedCommand s_queue[QUEUE_SIZE];
static unsigned s_head, s_count;
static uint32_t s_epoch = 1;
static bool s_direct_control;
static CmdCache s_cache[CACHE_SIZE];
static unsigned s_cache_next;
static PendingWrite s_pending;
static uint32_t s_next_token;
static uint32_t s_next_result;
static ble_cmd_write_fn s_write_fn;
static ble_cmd_read_fn s_read_fn;
static ble_cmd_battery_write_fn s_battery_write_fn;
static ble_cmd_battery_read_fn s_battery_read_fn;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void ble_ct_cmd_set_executors(ble_cmd_write_fn write_fn, ble_cmd_read_fn read_fn) {
    s_write_fn = write_fn;
    s_read_fn = read_fn;
}

void ble_ct_cmd_set_battery_executors(ble_cmd_battery_write_fn write_fn, ble_cmd_battery_read_fn read_fn) {
    s_battery_write_fn=write_fn;s_battery_read_fn=read_fn;
}

static const char *battery_error(int rc) {
    return rc==-14 ? "REVISION_CONFLICT" : rc==-15 ? "PROTOCOL_UNSUPPORTED" : rc==-16 ? "PROFILE_REQUIRED" :
        rc==-10 ? "INVALID_ARGS" : rc==-11 ? "UNSUPPORTED_PARAM" : rc==-12 ? "UPGRADE_BUSY" :
        rc==-13 ? "NOT_READY" : rc==-20 ? "STORAGE_FAILED" : rc==-21 ? "SUBMIT_FAILED" : "BUSY";
}

void ble_ct_cmd_reset_connection(void) {
    portENTER_CRITICAL(&s_rx_lock);
    ++s_epoch;
    s_head = s_count = 0;
    s_direct_control = false;
    ble_ct_set_framing_enabled(false);
    ble_ct_frames_reset(&s_rx);
    portEXIT_CRITICAL(&s_rx_lock);
}

static bool current_request(const Request *r) {
    bool current;
    portENTER_CRITICAL(&s_rx_lock);
    current = r->epoch == s_epoch;
    portEXIT_CRITICAL(&s_rx_lock);
    return current && strcmp(r->session, ble_ct_get_session_id()) == 0;
}

static bool valid_token(const cJSON *item) {
    if (!cJSON_IsString(item)) return false;
    size_t n = strlen(item->valuestring);
    return n > 0 && n <= TOKEN_MAX_LEN && strspn(item->valuestring,
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._:-") == n;
}

/* Bound cJSON recursion before parsing untrusted bytes on the main-task stack.
 * Brackets inside strings do not contribute to nesting. */
static bool bounded_json_depth(const uint8_t *message, size_t len) {
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < len; ++i) {
        uint8_t ch = message[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if (ch == '{' || ch == '[') {
            if (++depth > 8u) return false;
        } else if (ch == '}' || ch == ']') {
            if (!depth) return false;
            --depth;
        }
    }
    return !quoted && depth == 0;
}

/* Sort object keys recursively, excluding envelope/session/sequence/timestamp
 * from identity. A retry after reconnect is the same operation. */
static cJSON *canonical_copy(const cJSON *item) {
    if (!cJSON_IsObject(item) && !cJSON_IsArray(item)) return cJSON_Duplicate(item, true);
    cJSON *out = cJSON_IsObject(item) ? cJSON_CreateObject() : cJSON_CreateArray();
    if (!out) return NULL;
    const cJSON *child;
    const char *previous = NULL;
    int count = cJSON_GetArraySize(item);
    for (int i = 0; i < count; ++i) {
        child = cJSON_GetArrayItem(item, i);
        if (cJSON_IsObject(item)) {
            child = NULL;
            for (const cJSON *p = item->child; p; p = p->next) {
                if ((!previous || strcmp(p->string, previous) > 0) &&
                    (!child || strcmp(p->string, child->string) < 0)) child = p;
            }
            if (!child) { cJSON_Delete(out); return NULL; }
            previous = child->string;
        }
        cJSON *copy = canonical_copy(child);
        if (!copy) { cJSON_Delete(out); return NULL; }
        bool added = cJSON_IsObject(item) ? cJSON_AddItemToObject(out, child->string, copy)
                                         : cJSON_AddItemToArray(out, copy);
        if (!added) { cJSON_Delete(copy); cJSON_Delete(out); return NULL; }
    }
    return out;
}

static bool encode(const cJSON *json, char *out, size_t cap) {
    return json && cJSON_PrintPreallocated((cJSON *)json, out, (int)cap, false);
}

/* cJSON AddItem does not take ownership on failure. Every required field is
 * checked; callers must never treat a partially built object as a command. */
static bool add_owned(cJSON *object, const char *key, cJSON *item) {
    if (object && item && cJSON_AddItemToObject(object, key, item)) return true;
    cJSON_Delete(item);
    return false;
}

static cJSON *write_actual(const char *param_id, bool with_value, double value,
                           int saved, int submitted) {
    cJSON *actual = cJSON_CreateObject();
    if (!actual) return NULL;
    bool ok = cJSON_AddStringToObject(actual, "param_id", param_id) != NULL;
    if (with_value) ok = ok && cJSON_AddNumberToObject(actual, "value", value);
    ok = ok && (saved < 0 ? cJSON_AddNullToObject(actual, "saved") : cJSON_AddBoolToObject(actual, "saved", saved != 0));
    ok = ok && (submitted < 0 ? cJSON_AddNullToObject(actual, "submitted") : cJSON_AddBoolToObject(actual, "submitted", submitted != 0));
    ok = ok && cJSON_AddBoolToObject(actual, "observed", false);
    if (!ok) { cJSON_Delete(actual); return NULL; }
    return actual;
}

static cJSON *make_body(const Request *r, const char *status, const char *error,
                        cJSON *actual, uint32_t revision) {
    cJSON *body = cJSON_CreateObject();
    if (!body) { cJSON_Delete(actual); return NULL; }
    bool ok = true;
    if (r->v2) ok = cJSON_AddStringToObject(body, "operation_id", r->operation) != NULL;
    ok = ok && cJSON_AddStringToObject(body, "command_id", r->command);
    ok = ok && cJSON_AddStringToObject(body, "status", status);
    if (r->v2) {
        char result_id[24];
        snprintf(result_id, sizeof(result_id), "r-%lu", (unsigned long)++s_next_result);
        ok = ok && cJSON_AddStringToObject(body, "result_id", result_id);
    }
    if (error) {
        cJSON *err = ok ? cJSON_AddObjectToObject(body, "error") : NULL;
        ok = err && cJSON_AddStringToObject(err, "code", error);
    } else ok = ok && cJSON_AddNullToObject(body, "error");
    if (!ok) { cJSON_Delete(actual); cJSON_Delete(body); return NULL; }
    ok = add_owned(body, r->v2 ? "actual" : "data", actual ? actual : cJSON_CreateObject());
    if (r->v2) ok = ok && cJSON_AddNumberToObject(body, "parameter_revision", revision);
    if (!ok) { cJSON_Delete(body); return NULL; }
    return body;
}

static bool encode_result(const Request *r, const cJSON *body, char *out) {
    if (!r->v2) return encode(body, out, RESULT_CAPACITY + 1u);
    cJSON *envelope = cJSON_CreateObject();
    if (!envelope) return false;
    bool ok = cJSON_AddNumberToObject(envelope, "v", 2) &&
        cJSON_AddStringToObject(envelope, "type", "control.result");
    cJSON *result_id = cJSON_GetObjectItemCaseSensitive(body, "result_id");
    ok = ok && cJSON_IsString(result_id) && cJSON_AddStringToObject(envelope, "message_id", result_id->valuestring);
    ok = ok && cJSON_AddStringToObject(envelope, "in_reply_to", r->message);
    ok = ok && cJSON_AddStringToObject(envelope, "session_id", r->session);
    if (ok) ok = add_owned(envelope, "body", cJSON_Duplicate(body, true));
    ok = ok && encode(envelope, out, RESULT_CAPACITY + 1u);
    cJSON_Delete(envelope);
    return ok;
}

static void send_body(const Request *r, const cJSON *body) {
    char response[RESULT_CAPACITY + 1u];
    if (current_request(r) && encode_result(r, body, response))
        ble_ct_cmd_notify_result(response, (uint16_t)strlen(response));
}

static void reject(const Request *r, const char *code) {
    cJSON *body = make_body(r, r->v2 ? "rejected" : "error", code, NULL, 0);
    if (body) send_body(r, body);
    cJSON_Delete(body);
}

static CmdCache *cache_find(const char *id) {
    for (unsigned i = 0; i < CACHE_SIZE; ++i) {
        CmdCache *entry = &s_cache[i];
        if (entry->valid && now_ms() - entry->ts_ms > CACHE_EXPIRE_MS) entry->valid = false;
        if (entry->valid && strcmp(entry->command_id, id) == 0) return entry;
    }
    return NULL;
}

static bool cache_put(const Request *r, const uint8_t *canonical_digest, const cJSON *body) {
    char encoded[CACHE_BODY_CAPACITY + 1u];
    if (!encode(body, encoded, sizeof(encoded))) return false;
    CmdCache *entry = cache_find(r->command);
    if (!entry) {
        for (unsigned tries = 0; tries < CACHE_SIZE; ++tries) {
            entry = &s_cache[s_cache_next++ % CACHE_SIZE];
            if (!s_pending.active || strcmp(entry->command_id, s_pending.request.command) != 0) break;
        }
    }
    if (canonical_digest) memcpy(entry->canonical_digest, canonical_digest, sizeof(entry->canonical_digest));
    snprintf(entry->command_id, sizeof(entry->command_id), "%s", r->command);
    memcpy(entry->body, encoded, strlen(encoded) + 1u);
    entry->ts_ms = now_ms();
    entry->valid = true;
    return true;
}

uint32_t ble_ct_cmd_write_token(void) { return s_pending.active ? s_pending.token : 0; }

static void complete_write(const char *error, double value, uint32_t revision,
                            int saved, int submitted) {
    if (!s_pending.active) return;
    cJSON *actual = write_actual(s_pending.param_id, !error && strcmp(s_pending.param_id,"battery_config"), value, saved, submitted);
    if (!actual) return;
    cJSON *body = make_body(&s_pending.request, error ? "failed" : "saved", error, actual, revision);
    if (body) {
        cache_put(&s_pending.request, NULL, body);
        send_body(&s_pending.request, body);
        cJSON_Delete(body);
    }
    s_pending.active = false;
}

void ble_ct_cmd_on_write_ack(uint32_t token, const char *param_id, int err) {
    if (!s_pending.active || token != s_pending.token || !param_id ||
        strcmp(param_id, s_pending.param_id) != 0 || err == 0) return;
    bool save_failed=err==BLE_WRITE_SAVE_FAILED || err==-2;
    complete_write(save_failed ? "STORAGE_FAILED" : err==BLE_WRITE_SUBMIT_FAILED ? "SUBMIT_FAILED" : err==-1 ? "SYNC_TIMEOUT" : "RESULT_UNAVAILABLE",
        0, 0, save_failed ? 0 : 1, err == -3 ? 1 : 0);
}

void ble_ct_cmd_on_write_saved(uint32_t token, const char *param_id, double actual,
                                uint32_t parameter_revision) {
    if (!s_pending.active || token != s_pending.token || !param_id ||
        strcmp(param_id, s_pending.param_id) != 0 || !isfinite(actual)) return;
    complete_write(NULL, actual, parameter_revision, 1, 1);
}

static bool parse_request(cJSON *root, Request *r, cJSON **command) {
    if (!cJSON_IsObject(root)) return false;
    *command = root;
    snprintf(r->session, sizeof(r->session), "%s", ble_ct_get_session_id());
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "v");
    if (version) {
        if (!cJSON_IsNumber(version) || version->valuedouble != 2) return false;
        cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
        cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message_id");
        cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session_id");
        cJSON *body = cJSON_GetObjectItemCaseSensitive(root, "body");
        cJSON *body_session = cJSON_GetObjectItemCaseSensitive(body, "session_id");
        cJSON *operation = cJSON_GetObjectItemCaseSensitive(body, "operation_id");
        cJSON *sequence = cJSON_GetObjectItemCaseSensitive(body, "sequence");
        if (!valid_token(type) || strcmp(type->valuestring, "control.request") ||
            !valid_token(message) || !valid_token(session) || !cJSON_IsObject(body) ||
            !valid_token(body_session) || !valid_token(operation) ||
            strcmp(session->valuestring, r->session) || strcmp(body_session->valuestring, r->session) ||
            !cJSON_IsNumber(sequence) || !isfinite(sequence->valuedouble) ||
            sequence->valuedouble < 1 || sequence->valuedouble > UINT32_MAX ||
            floor(sequence->valuedouble) != sequence->valuedouble) return false;
        r->v2 = true;
        snprintf(r->message, sizeof(r->message), "%s", message->valuestring);
        snprintf(r->operation, sizeof(r->operation), "%s", operation->valuestring);
        *command = body;
    }
    cJSON *id = cJSON_GetObjectItemCaseSensitive(*command, "command_id");
    cJSON *action = cJSON_GetObjectItemCaseSensitive(*command, "action");
    cJSON *params = cJSON_GetObjectItemCaseSensitive(*command, "params");
    cJSON *pid = cJSON_GetObjectItemCaseSensitive(params, "param_id");
    cJSON *ts = cJSON_GetObjectItemCaseSensitive(*command, "ts");
    if (!valid_token(id) || !valid_token(action) || (params && !cJSON_IsObject(params)) ||
        (pid && !valid_token(pid)) ||
        (ts && (!cJSON_IsNumber(ts) || !isfinite(ts->valuedouble) || ts->valuedouble < 0 ||
                ts->valuedouble > UINT32_MAX || floor(ts->valuedouble) != ts->valuedouble))) return false;
    snprintf(r->command, sizeof(r->command), "%s", id->valuestring);
    return true;
}

static void dispatch(const QueuedCommand *queued) {
    Request r = {.epoch = queued->epoch};
    cJSON *root = cJSON_ParseWithLengthOpts(queued->json, strlen(queued->json) + 1u, NULL, true);
    cJSON *command = NULL;
    if (!root || !parse_request(root, &r, &command) || !current_request(&r)) {
        cJSON_Delete(root); return;
    }
    cJSON *action_item = cJSON_GetObjectItemCaseSensitive(command, "action");
    const char *action = action_item->valuestring;
    cJSON *params = cJSON_GetObjectItemCaseSensitive(command, "params");
    cJSON *pid = cJSON_GetObjectItemCaseSensitive(params, "param_id");
    cJSON *value = cJSON_GetObjectItemCaseSensitive(params, "value");
    bool battery_write=!strcmp(action,"set_battery_config");
    bool battery_read=!strcmp(action,"get_battery_config");
    cJSON *battery_config=params;
    if (battery_write) {
        cJSON *wrapped=cJSON_GetObjectItemCaseSensitive(params,"config");
        if (wrapped) {
            if (!params->child || params->child->next || params->child!=wrapped) { reject(&r,"INVALID_ARGS");cJSON_Delete(root);return; }
            battery_config=wrapped;
        }
        int rc=s_battery_write_fn ? s_battery_write_fn(battery_config,true) : -13;
        if (rc) { reject(&r,battery_error(rc));cJSON_Delete(root);return; }
    }
    if (battery_read && params && (!cJSON_IsObject(params) || params->child)) { reject(&r,"INVALID_ARGS");cJSON_Delete(root);return; }
    cJSON *canonical_json = cJSON_CreateObject();
    bool canonical_ok = canonical_json && cJSON_AddStringToObject(canonical_json, "action", action);
    if (canonical_ok) canonical_ok = add_owned(canonical_json, "params", battery_write ? canonical_copy(battery_config) : params ? canonical_copy(params) : cJSON_CreateObject());
    char canonical[COMMAND_CAPACITY + 1u];
    if (!canonical_ok ||
        !encode(canonical_json, canonical, sizeof(canonical))) {
        reject(&r, "INVALID_ARGS"); goto done;
    }
    /* Keep the eight replay results without retaining eight full command
     * copies. Hash the same canonical bytes before cache lookup or execution,
     * so a hash failure cannot admit a write or alter replay state. */
    uint8_t canonical_digest[COMMAND_DIGEST_SIZE];
    if (mbedtls_sha256((const unsigned char *)canonical, strlen(canonical), canonical_digest, 0) != 0) {
        reject(&r, "RESULT_UNAVAILABLE"); goto done;
    }
    if (!strcmp(action, "power_on") || !strcmp(action, "power_off")) {
        reject(&r, "UNSUPPORTED_ACTION"); goto done;
    }
    if (!strcmp(action, "set_param") || battery_write) {
        bool enabled;
        portENTER_CRITICAL(&s_rx_lock); enabled = s_direct_control && r.epoch == s_epoch; portEXIT_CRITICAL(&s_rx_lock);
        if (!r.v2 || !enabled || !ble_ct_framing_enabled()) { reject(&r, "UPDATE_APP_REQUIRED"); goto done; }
    }
    bool opening = !strcmp(action, "open_session");
    cJSON *frame_flag = cJSON_GetObjectItemCaseSensitive(params, "framing_v1");
    cJSON *control_flag = cJSON_GetObjectItemCaseSensitive(params, "direct_control_v1");
    if (opening && (!r.v2 || !cJSON_IsBool(frame_flag) || !cJSON_IsTrue(frame_flag) || !cJSON_IsBool(control_flag))) {
        reject(&r, "INVALID_ARGS"); goto done;
    }
    CmdCache *cached = cache_find(r.command);
    if (cached && memcmp(cached->canonical_digest, canonical_digest, sizeof(canonical_digest))) { reject(&r, "IDEMPOTENCY_CONFLICT"); goto done; }
    if (cached) {
        cJSON *body = cJSON_Parse(cached->body);
        if (body) {
            if (r.v2) {
                cJSON *operation = cJSON_CreateString(r.operation);
                if (!operation || !cJSON_ReplaceItemInObjectCaseSensitive(body, "operation_id", operation)) {
                    cJSON_Delete(operation); cJSON_Delete(body); goto done;
                }
            }
            char *replay=canonical; /* canonical bytes are no longer needed after hashing */
            if (!encode_result(&r, body, replay)) {
                cJSON_Delete(body); reject(&r, "RESULT_TOO_LARGE"); goto done;
            }
            if (opening) {
                portENTER_CRITICAL(&s_rx_lock);
                if (r.epoch == s_epoch) {
                    s_direct_control = cJSON_IsTrue(control_flag);
                    ble_ct_set_framing_enabled(true);
                }
                portEXIT_CRITICAL(&s_rx_lock);
            }
            if (current_request(&r)) ble_ct_cmd_notify_result(replay, (uint16_t)strlen(replay));
            cJSON_Delete(body);
        }
        goto done;
    }
    cJSON *ts = cJSON_GetObjectItemCaseSensitive(command, "ts");
    uint32_t wall_now = ble_ct_now();
    if (ts && wall_now && fabs((double)wall_now - ts->valuedouble) > 300) { reject(&r, "TIMESTAMP_OUT_OF_RANGE"); goto done; }
    cJSON *actual = cJSON_CreateObject();
    if (!actual) goto done;
    cJSON *body = NULL;
    if (opening) {
        if (!cJSON_AddBoolToObject(actual, "framing_v1", true) ||
            !cJSON_AddBoolToObject(actual, "direct_control_v1", cJSON_IsTrue(control_flag)) ||
            !cJSON_AddNumberToObject(actual,"battery_config_max_message_bytes",COMMAND_CAPACITY)) {
            cJSON_Delete(actual); goto done;
        }
        body = make_body(&r, "applied", NULL, actual, 0);
    } else if (!strcmp(action, "get_param")) {
        double read_value;
        if (!valid_token(pid) || !s_read_fn) { cJSON_Delete(actual); reject(&r, "INVALID_ARGS"); goto done; }
        if (s_read_fn(pid->valuestring, &read_value) != BLE_CMD_OK || !isfinite(read_value)) {
            cJSON_Delete(actual); reject(&r, "UNKNOWN_PARAM"); goto done;
        }
        if (!cJSON_AddStringToObject(actual, "param_id", pid->valuestring) ||
            !cJSON_AddNumberToObject(actual, "value", read_value)) { cJSON_Delete(actual); goto done; }
        body = make_body(&r, r.v2 ? "applied" : "ok", NULL, actual, 0);
    } else if (battery_read) {
        cJSON *config=s_battery_read_fn ? s_battery_read_fn() : NULL;
        cJSON *revision=cJSON_GetObjectItemCaseSensitive(config,"revision");
        if (!cJSON_IsObject(config) || !cJSON_IsNumber(revision)) {
            cJSON_Delete(config);cJSON_Delete(actual);reject(&r,"NOT_READY");goto done;
        }
        uint32_t rev=(uint32_t)revision->valuedouble;
        if (!add_owned(actual,"config",config)) { cJSON_Delete(actual);goto done; }
        body=make_body(&r,r.v2 ? "completed" : "ok",NULL,actual,rev);
        /* The exact token already lives in actual.config.revision. Avoid a
         * duplicate to keep even 64-byte IDs in the existing 512-byte cache. */
        if (body && r.v2) cJSON_DeleteItemFromObjectCaseSensitive(body,"parameter_revision");
    } else if (!strcmp(action, "set_param") || battery_write) {
        cJSON_Delete(actual);
        if (!battery_write && (!valid_token(pid) || !cJSON_IsNumber(value) || !isfinite(value->valuedouble) || !s_write_fn)) {
            reject(&r, "INVALID_ARGS"); goto done;
        }
        if (s_pending.active) { reject(&r, "BUSY"); goto done; }
        /* Prove accepted AND final results fit before touching storage/UART. */
        const char *param_id=battery_write ? "battery_config" : pid->valuestring;
        actual = write_actual(param_id, !battery_write, battery_write ? 0 : value->valuedouble, 1, 1);
        if (!actual) goto done;
        /* Include the longest failure code as well as actual-value fields so
         * later failures cannot overflow after a successful preflight. */
        body = make_body(&r, "saved", "RESULT_UNAVAILABLE", actual, UINT32_MAX);
        char *preflight=canonical;
        char cached_completion[CACHE_BODY_CAPACITY + 1u];
        bool fits = body && encode(body,cached_completion,sizeof(cached_completion)) && encode_result(&r, body, preflight);
        cJSON_Delete(body);
        if (!fits) { reject(&r, "RESULT_TOO_LARGE"); goto done; }
        body = make_body(&r, "accepted", NULL, NULL, 0);
        if (!body || !encode_result(&r, body, preflight) || !cache_put(&r, canonical_digest, body)) {
            cJSON_Delete(body); reject(&r, "RESULT_TOO_LARGE"); goto done;
        }
        s_pending.active = true;
        s_pending.request = r;
        s_pending.started_ms = now_ms();
        if (++s_next_token == 0) ++s_next_token;
        s_pending.token = s_next_token;
        snprintf(s_pending.param_id, sizeof(s_pending.param_id), "%s", param_id);
        /* Cache accepted BEFORE executor; synchronous completion stays final. */
        /* preflight already contains the complete accepted envelope; do not
         * allocate again between admission and the executor. */
        if (current_request(&r)) ble_ct_cmd_notify_result(preflight, (uint16_t)strlen(preflight));
        cJSON_Delete(body);
        int rc = battery_write ? s_battery_write_fn(battery_config,false) : s_write_fn(pid->valuestring, value->valuedouble);
        if (rc != BLE_CMD_OK && s_pending.active) {
            const char *error=battery_write || rc<0 ? battery_error(rc) : rc==BLE_CMD_BUSY ? "BUSY" : rc==BLE_CMD_OUT_OF_RANGE ? "OUT_OF_RANGE" :
                rc==BLE_CMD_UNSUPPORTED ? "UNSUPPORTED_PARAM" : rc==BLE_CMD_UPGRADE_BUSY ? "UPGRADE_BUSY" :
                rc==BLE_CMD_NOT_READY ? "NOT_READY" : "UNKNOWN_PARAM";
            complete_write(error, 0, 0, 0, 0);
        }
        goto done;
    } else { cJSON_Delete(actual); reject(&r, "UNSUPPORTED_ACTION"); goto done; }
    if (body) {
        char *preflight=canonical;
        if (encode_result(&r, body, preflight) && cache_put(&r, canonical_digest, body)) {
            /* Commit negotiation only after validation and a replayable result
             * exist. A rejected open must leave connection state untouched. */
            if (opening) {
                portENTER_CRITICAL(&s_rx_lock);
                if (r.epoch == s_epoch) {
                    s_direct_control = cJSON_IsTrue(control_flag);
                    ble_ct_set_framing_enabled(true);
                }
                portEXIT_CRITICAL(&s_rx_lock);
            }
            if (current_request(&r)) ble_ct_cmd_notify_result(preflight, (uint16_t)strlen(preflight));
        } else reject(&r, "RESULT_TOO_LARGE");
        cJSON_Delete(body);
    }
done:
    cJSON_Delete(canonical_json);
    cJSON_Delete(root);
}

void ble_ct_cmd_tick(void) {
    if (s_pending.active && now_ms() - s_pending.started_ms >= WRITE_TIMEOUT_MS)
        complete_write("SYNC_TIMEOUT", 0, 0, -1, -1);
    QueuedCommand queued;
    bool available = false;
    portENTER_CRITICAL(&s_rx_lock);
    if (s_count) {
        queued = s_queue[s_head];
        s_head = (s_head + 1u) % QUEUE_SIZE;
        --s_count;
        available = true;
    }
    portEXIT_CRITICAL(&s_rx_lock);
    if (available) dispatch(&queued);
}

int ble_ct_cmd_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || conn_handle != ble_ct_get_conn_handle()) return BLE_ATT_ERR_UNLIKELY;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    uint8_t bytes[COMMAND_CAPACITY + 1u];
    if (!len || len > COMMAND_CAPACITY || ble_hs_mbuf_to_flat(ctxt->om, bytes, len, NULL))
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    int error = 0;
    portENTER_CRITICAL(&s_rx_lock);
    const uint8_t *message = bytes;
    size_t message_len = len;
    if (bytes[0] != '{') {
        int rc = ble_ct_frames_accept(&s_rx, bytes, len, now_ms(), RECEIVE_TIMEOUT_MS, COMMAND_CAPACITY);
        if (rc == BLE_CT_FRAME_ERROR) error = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (rc != BLE_CT_FRAME_COMPLETE) { portEXIT_CRITICAL(&s_rx_lock); return error; }
        message = s_rx.data;
        message_len = s_rx.len;
    } else ble_ct_frames_reset(&s_rx);
    if (!message_len || memchr(message, 0, message_len) || !bounded_json_depth(message, message_len)) error = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    else if (s_count == QUEUE_SIZE) error = BLE_ATT_ERR_INSUFFICIENT_RES;
    else {
        QueuedCommand *queued = &s_queue[(s_head + s_count) % QUEUE_SIZE];
        memcpy(queued->json, message, message_len);
        queued->json[message_len] = 0;
        if (strstr(queued->json, "\\u0000")) error = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        else { queued->epoch = s_epoch; ++s_count; }
    }
    ble_ct_frames_reset(&s_rx);
    portEXIT_CRITICAL(&s_rx_lock);
    return error;
}

void ble_ct_cmd_notify_result(const char *json, uint16_t len) {
    if (!json || !len || len > RESULT_CAPACITY || !ble_prov_host_acquire()) return;
    uint16_t conn = ble_ct_get_conn_handle();
    if (conn == BLE_HS_CONN_HANDLE_NONE || !ble_ct_is_result_subscribed()) {
        ble_prov_host_release(); return;
    }
    uint16_t mtu = ble_att_mtu(conn);
    if (ble_ct_framing_enabled()) {
        size_t offset = 0, frame_len = 0;
        uint8_t seq = 0, frame[512];
        int rc;
        do {
            rc = ble_ct_frames_next((const uint8_t *)json, len, mtu, &offset, &seq, frame, sizeof(frame), &frame_len);
            if (rc < 0) break;
            struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, (uint16_t)frame_len);
            if (!om || ble_gattc_notify_custom(conn, ble_ct_get_result_handle(), om)) break;
        } while (rc == BLE_CT_FRAME_MORE);
    } else if (mtu >= 3u && len <= mtu - 3u) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(json, len);
        if (om) ble_gattc_notify_custom(conn, ble_ct_get_result_handle(), om);
    } else {
        static const char fallback[] = "{\"error\":\"MTU\"}";
        if (mtu >= 3u && sizeof(fallback) - 1u <= mtu - 3u) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(fallback, sizeof(fallback) - 1u);
            if (om) ble_gattc_notify_custom(conn, ble_ct_get_result_handle(), om);
        }
    }
    ble_prov_host_release();
}
