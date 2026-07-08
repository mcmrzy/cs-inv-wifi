#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include "config.h"

void state_machine_init(void);
void state_machine_run(void);

void on_uart_frame(uint8_t cmd, const uint8_t *data, uint16_t len);
void on_provision_done(const char *ssid, const char *password);
void on_mqtt_message(const char *topic, const char *payload, unsigned int len);

void check_button(void);
void build_ap_ssid(char *out, size_t out_len);

extern char g_device_sn[SN_MAX_LEN];
extern char g_device_key[65];
extern char g_wifi_ssid[33];
extern char g_wifi_pass[65];
extern enum SystemState g_sys_state;

#endif
