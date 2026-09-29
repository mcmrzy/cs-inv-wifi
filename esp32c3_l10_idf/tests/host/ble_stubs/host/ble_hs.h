#pragma once
#include <stdint.h>
#include <stddef.h>
struct os_mbuf { const void *data; uint16_t len; };
#define OS_MBUF_PKTLEN(om) ((om)->len)
#define BLE_HS_CONN_HANDLE_NONE UINT16_MAX
#define BLE_ATT_ERR_UNLIKELY 14
#define BLE_ATT_ERR_INSUFFICIENT_AUTHEN 5
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 13
#define BLE_ATT_ERR_PREPARE_QUEUE_FULL 9
struct ble_gap_conn_desc { struct { uint8_t val[6]; } peer_id_addr; };
int ble_gap_conn_find(uint16_t conn, struct ble_gap_conn_desc *desc);
int ble_hs_mbuf_to_flat(const struct os_mbuf *om, void *dst, uint16_t max_len, uint16_t *out_len);
struct os_mbuf *ble_hs_mbuf_from_flat(const void *data, uint16_t len);
int os_mbuf_append(struct os_mbuf *om, const void *data, uint16_t len);
void os_mbuf_free_chain(struct os_mbuf *om);
void os_mbuf_concat(struct os_mbuf *first, struct os_mbuf *second);
#include "ble_gatt.h"
