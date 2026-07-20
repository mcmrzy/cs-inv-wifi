#ifndef AP_API_SERVER_H
#define AP_API_SERVER_H

#include <stdbool.h>

typedef bool (*ApProvisionCallback)(const char* ssid, const char* password);

void ap_api_server_set_provision_callback(ApProvisionCallback callback);
bool ap_api_server_start(void);
void ap_api_server_stop(void);

#endif
