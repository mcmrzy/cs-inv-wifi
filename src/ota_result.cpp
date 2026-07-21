#include "ota_result.h"
#include <Preferences.h>
#include <string.h>

static Preferences prefs;

void ota_result_save(const char* status, const char* version, const char* message) {
    prefs.begin("ota_result", false);
    prefs.putString("status", status);
    prefs.putString("version", version);
    prefs.putString("message", message);
    prefs.putBool("valid", true);
    prefs.remove("task_id");
    prefs.remove("target");
    prefs.end();
}

void ota_task_result_save(const char* task_id, const char* target,
                          const char* status, const char* version, const char* message) {
    prefs.begin("ota_result", false);
    prefs.putString("status", status ? status : "");
    prefs.putString("version", version ? version : "");
    prefs.putString("message", message ? message : "");
    prefs.putString("task_id", task_id ? task_id : "");
    prefs.putString("target", target ? target : "");
    prefs.putBool("valid", true);
    prefs.end();
}

bool ota_result_load(OtaResult* result) {
    if (!result) return false;
    memset(result, 0, sizeof(*result));
    prefs.begin("ota_result", true);
    bool valid = prefs.getBool("valid", false);
    if (!valid) {
        prefs.end();
        return false;
    }
    strncpy(result->status, prefs.getString("status", "").c_str(), sizeof(result->status) - 1);
    strncpy(result->version, prefs.getString("version", "").c_str(), sizeof(result->version) - 1);
    strncpy(result->message, prefs.getString("message", "").c_str(), sizeof(result->message) - 1);
    strncpy(result->task_id, prefs.getString("task_id", "").c_str(), sizeof(result->task_id) - 1);
    strncpy(result->target, prefs.getString("target", "").c_str(), sizeof(result->target) - 1);
    prefs.end();
    return true;
}

void ota_result_clear() {
    prefs.begin("ota_result", false);
    prefs.clear();
    prefs.end();
}

uint32_t ota_security_version_get() {
    Preferences security;
    if (!security.begin("ota_security", true)) return 0;
    uint32_t version = security.getUInt("version", 0);
    security.end();
    return version;
}

bool ota_security_version_advance(uint32_t version) {
    Preferences security;
    if (!security.begin("ota_security", false)) return false;
    uint32_t current = security.getUInt("version", 0);
    bool ok = version > current &&
              security.putUInt("version", version) == sizeof(version);
    security.end();
    return ok;
}
