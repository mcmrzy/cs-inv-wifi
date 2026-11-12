#pragma once
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
typedef void *rmt_channel_handle_t;
typedef void *rmt_encoder_handle_t;
typedef union {struct {uint32_t duration0:15,level0:1,duration1:15,level1:1;};uint32_t val;} rmt_symbol_word_t;
typedef struct {int gpio_num,clk_src;uint32_t resolution_hz;size_t mem_block_symbols,trans_queue_depth;} rmt_tx_channel_config_t;
typedef struct {} rmt_copy_encoder_config_t;
typedef struct {int loop_count;struct {unsigned eot_level;} flags;} rmt_transmit_config_t;
enum {ESP_OK,ESP_ERR_TIMEOUT,ESP_FAIL,RMT_CLK_SRC_DEFAULT};
#define CONFIG_PANEL_LED_BRIGHTNESS 24
void mock_log(const char *,const char *,...);
#define ESP_LOGW mock_log
#define ESP_LOGI mock_log
int64_t esp_timer_get_time(void);
const char *esp_err_to_name(int);
esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t *,rmt_channel_handle_t *);
esp_err_t rmt_new_copy_encoder(const rmt_copy_encoder_config_t *,rmt_encoder_handle_t *);
esp_err_t rmt_enable(rmt_channel_handle_t);
esp_err_t rmt_del_channel(rmt_channel_handle_t);
esp_err_t rmt_del_encoder(rmt_encoder_handle_t);
esp_err_t rmt_transmit(rmt_channel_handle_t,rmt_encoder_handle_t,const void *,size_t,const rmt_transmit_config_t *);
esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t,int);
bool panel_bus_snapshot(panel_snapshot_t *);
uint32_t panel_bus_context_generation(void);
