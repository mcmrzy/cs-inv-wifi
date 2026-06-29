#ifndef TCP_PROVISION_H
#define TCP_PROVISION_H

#include "config.h"
#include <WiFi.h>

// 配网结果回调
typedef void (*ProvisionCallback)(const char* ssid, const char* password);

void tcp_provision_init();
void tcp_provision_set_callback(ProvisionCallback cb);
void tcp_provision_process();
void tcp_provision_stop();

#endif
