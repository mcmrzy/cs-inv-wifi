#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define DIAG_LINE_MAX 1024U
#define DIAG_TX_MAX 256U
#define DIAG_RX_MAX 1024U
typedef enum {
    CMD_HELP,CMD_STATUS,CMD_STATS,CMD_CLEAR,CMD_REBOOT,CMD_STOP,
    CMD_BAUD,CMD_PINS,CMD_SWAP,CMD_ADDR,CMD_FORMAT,CMD_FC,CMD_TIMEOUT,
    CMD_VERBOSE,CMD_INVERT,CMD_READ,CMD_POLL,CMD_SCAN,CMD_HUNT,
    CMD_LISTEN,CMD_TX,CMD_TXCRC,CMD_CRC,CMD_LOOPBACK,CMD_LEVELS,CMD_LEGACY
} diag_kind_t;
typedef struct {
    diag_kind_t kind;
    uint32_t arg[4];
    char parity;
    uint8_t data[DIAG_TX_MAX];
    size_t length;
} diag_command_t;
typedef enum {DIAG_NO_RX,DIAG_ECHO,DIAG_CRC_BAD,DIAG_UNMATCHED,
              DIAG_READ_OK,DIAG_EXCEPTION,DIAG_LEGACY_OK} diag_result_t;
typedef struct {
    diag_result_t result;
    bool echo;
    uint8_t exception;
    size_t offset,length;
    uint16_t count,values[125];
} diag_reply_t;
bool diag_command(const char *line,diag_command_t *out);
size_t diag_read_request(uint8_t address,uint8_t function,uint16_t reg,uint16_t count,uint8_t out[8]);
size_t diag_legacy_request(uint8_t out[9]);
diag_reply_t diag_analyze(const uint8_t *tx,size_t txlen,const uint8_t *rx,size_t rxlen,
                         uint8_t address,uint8_t function,uint16_t count,bool legacy);
const char *diag_result_name(diag_result_t result);
