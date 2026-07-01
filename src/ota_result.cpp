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
    prefs.end();
}

bool ota_result_load(OtaResult* result) {
    prefs.begin("ota_result", true);
    bool valid = prefs.getBool("valid", false);
    if (!valid) {
        prefs.end();
        return false;
    }
    strncpy(result->status, prefs.getString("status", "").c_str(), sizeof(result->status) - 1);
    strncpy(result->version, prefs.getString("version", "").c_str(), sizeof(result->version) - 1);
    strncpy(result->message, prefs.getString("message", "").c_str(), sizeof(result->message) - 1);
    prefs.end();
    return true;
}

void ota_result_clear() {
    prefs.begin("ota_result", false);
    prefs.clear();
    prefs.end();
}