#ifndef TCP_PROVISION_H
#define TCP_PROVISION_H

#include "config.h"

typedef void (*ProvisionCallback)(const char* ssid, const char* password);

void tcp_provision_init(void);
void tcp_provision_set_callback(ProvisionCallback cb);
void tcp_provision_process(void);
void tcp_provision_stop(void);

#endif
