#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "direct_protocol.h"
#include "direct_lcd_codec.h"
#include "direct_board_pins.h"

static void append_crc(uint8_t *frame, size_t len)
{
    uint16_t crc = direct_crc16(frame, len - 2);
    frame[len - 2] = (uint8_t)crc;
    frame[len - 1] = (uint8_t)(crc >> 8);
}

int main(void)
{
    assert(!direct_lcd_cs_toggles(-1,false));
    assert(!direct_lcd_cs_toggles(4,true));
    assert(direct_lcd_cs_toggles(4,false));
    /* Regression: ARM PB14/SPI2_MISO maps to GPIO9 as LCD D/C. */
    assert(direct_board_gpio_allowed(9, DIRECT_PIN_LCD_DC));
    assert(!direct_board_gpio_allowed(9, DIRECT_PIN_OTHER));
    assert(!direct_board_gpio_allowed(9, DIRECT_PIN_KEY_CLOCK));
    assert(direct_board_gpio_allowed(2, DIRECT_PIN_KEY_CLOCK));
    assert(!direct_board_gpio_allowed(2, DIRECT_PIN_LCD_DC));
    assert(!direct_board_gpio_allowed(18, DIRECT_PIN_LCD_DC));
    assert(!direct_board_gpio_allowed(19, DIRECT_PIN_OTHER));
    assert(!direct_board_gpio_allowed(8, DIRECT_PIN_OTHER));
    assert(direct_board_gpio_allowed(-1, DIRECT_PIN_OTHER));
    uint8_t packed[3];
    const uint8_t lcd_command = 0x2a;
    assert(direct_lcd_pack9(&lcd_command, 1, false, packed,
                            sizeof(packed)) == 9);
    assert(packed[0] == 0x15 && packed[1] == 0x00);
    const uint8_t lcd_data = 0x55;
    assert(direct_lcd_pack9(&lcd_data, 1, true, packed,
                            sizeof(packed)) == 9);
    assert(packed[0] == 0xaa && packed[1] == 0x80);
    const uint8_t lcd_pair[2] = {0x12, 0x34};
    assert(direct_lcd_pack9(lcd_pair, 2, true, packed,
                            sizeof(packed)) == 18);
    assert(packed[0] == 0x89 && packed[1] == 0x4d && packed[2] == 0x00);
    assert(direct_lcd_pack9(&lcd_data, 1, true, packed, 1) == 0);

    uint8_t query[8];
    assert(direct_bms_make_query(1, 0, 0x25, query) == 8);
    const uint8_t arm_query[8] = {1, 3, 0, 0, 0, 0x25, 0x84, 0x11};
    assert(memcmp(query, arm_query, sizeof(query)) == 0);
    assert(direct_bms_make_query(0, 0, 0x25, query) == 0);

    uint8_t request[9];
    uint8_t status_frame[22]={1,18,3};
    status_frame[3]=0x12; status_frame[6]=0x34;
    status_frame[7]=0x40; status_frame[8]=0x81;
    status_frame[13]=0x11; status_frame[15]=0x22; status_frame[17]=0x33;
    status_frame[19]=16;
    append_crc(status_frame,sizeof(status_frame));
    direct_dsp_status_t status;
    assert(direct_dsp_parse_cmd03(status_frame,sizeof(status_frame),&status));
    assert(status.fault==0x12000034 && status.inverter_status[0]==0x4081 &&
           status.warning[0]==0x1100 && status.warning[1]==0x2200 &&
           status.warning[2]==0x3300 && status.mode==16);
    assert(!direct_dsp_parse_cmd03(status_frame,21,&status));
    uint8_t config_frame[55]={1,51,0x25};
    config_frame[3]=1; config_frame[4]=3;
    config_frame[7]=0x08; config_frame[8]=0xfc;
    config_frame[9]=0x13; config_frame[10]=0x88;
    config_frame[11]=80; config_frame[12]=4;
    config_frame[19]=0x01; config_frame[20]=0xe0;
    config_frame[21]=0x02; config_frame[22]=0x1c;
    config_frame[23]=10; config_frame[24]=20; config_frame[25]=80;
    config_frame[26]=30; config_frame[40]=1;
    append_crc(config_frame,sizeof(config_frame));
    uint16_t config_values[DIRECT_DSP_CONFIG_COUNT]={0};
    bool config_valid[DIRECT_DSP_CONFIG_COUNT]={0};
    assert(direct_dsp_parse_cmd25(config_frame,sizeof(config_frame),config_values,config_valid));
    assert(config_values[7]==2300 && config_values[8]==5000 && config_values[48]==1);
    assert(config_valid[0] && config_valid[48] && !config_valid[5]);
    uint8_t write_frame[16];
    size_t written=direct_dsp_make_config_write(7,2400,config_values,config_valid,write_frame);
    assert(written==7 && write_frame[2]==7 && write_frame[3]==0x09 && write_frame[4]==0x60);
    assert(direct_dsp_validate(write_frame,written));
    assert(direct_dsp_make_config_write(7,65535,config_values,config_valid,write_frame)==0);
    written=direct_dsp_make_config_write(1,90,config_values,config_valid,write_frame);
    assert(written==9 && write_frame[2]==0x17 && write_frame[4]==90 && write_frame[6]==30);
    assert(direct_dsp_validate(write_frame,written));
    config_valid[10]=false;
    assert(direct_dsp_make_config_write(1,90,config_values,config_valid,write_frame)==0);
    assert(!direct_dsp_config_writable(5));
    written=direct_dsp_make_config_write(0,4,config_values,config_valid,write_frame);
    assert(written==7 && write_frame[2]==0x18 && write_frame[3]==4 && write_frame[4]==3);
    assert(direct_dsp_make_config_write(13,2,config_values,config_valid,write_frame)==0);
    uint8_t charge_frame[11];
    assert(direct_dsp_make_charge_write(580,560,440,charge_frame)==11);
    assert(charge_frame[2]==0x2f && charge_frame[3]==2 && charge_frame[4]==0x44 &&
           direct_dsp_validate(charge_frame,sizeof(charge_frame)));
    assert(direct_dsp_make_charge_write(580,560,570,charge_frame)==0);
    config_valid[67]=true; config_valid[69]=true; config_valid[74]=true;
    assert(direct_dsp_parse_cmd25(config_frame,sizeof(config_frame),config_values,config_valid));
    assert(!config_valid[67] && !config_valid[69] && !config_valid[74]);
    assert(config_valid[72] && config_valid[78]);
    config_frame[12]=5; append_crc(config_frame,sizeof(config_frame));
    assert(direct_dsp_parse_cmd25(config_frame,sizeof(config_frame),config_values,config_valid));
    for (unsigned i=67;i<=81;++i) assert(!config_valid[i]);
    config_frame[40]^=1;
    assert(!direct_dsp_parse_cmd25(config_frame,sizeof(config_frame),config_values,config_valid));
    assert(direct_dsp_make_request(0x05, -1000, 0, 0, request) == 9);
    assert(request[3] == 0xff && request[4] == 0xff &&
           request[5] == 0xfc && request[6] == 0x18);
    assert(direct_dsp_validate(request, sizeof(request)));
    request[8] ^= 1;
    assert(!direct_dsp_validate(request, sizeof(request)));
    assert(!direct_dsp_validate(request, sizeof(request) - 1));

    uint8_t dsp[DIRECT_DSP_CMD04_LEN] = {0};
    dsp[0] = 1; dsp[1] = 65; dsp[2] = 4;
    dsp[3] = 0x08; dsp[4] = 0x98;   /* grid 2200 raw */
    dsp[41] = 0xff; dsp[42] = 0x9c; /* -100 (0.1 A) */
    dsp[59] = 0x01; dsp[60] = 0x2c; /* PV1 300 W */
    dsp[63] = 0x01; dsp[64] = 0x2c; /* total 300 W */
    append_crc(dsp, sizeof(dsp));
    direct_dsp_measurement_t m;
    assert(direct_dsp_parse_cmd04(dsp, sizeof(dsp), &m));
    assert(m.grid_v == 2200 && m.battery_i_tenths == -100 &&
           m.pv1_w == 300 && m.pv_total_w == 300);
    dsp[40] ^= 1;
    assert(!direct_dsp_parse_cmd04(dsp, sizeof(dsp), &m));

    uint8_t bms[79] = {0};
    bms[0] = 1; bms[1] = 3; bms[2] = 0x4a;
    bms[5] = 0x14; bms[6] = 0x32;
    bms[7] = 0; bms[8] = 80;
    bms[37] = 0x0c; bms[38] = 0xe4;
    append_crc(bms, sizeof(bms));
    direct_bms_measurement_t bm;
    assert(direct_bms_parse_modbus(bms, sizeof(bms), 1, &bm));
    assert(bm.voltage_10mv == 0x1432 && bm.soc_percent == 80 &&
           bm.cell_mv[0] == 3300);
    assert(!direct_bms_parse_modbus(bms, sizeof(bms) - 1, 1, &bm));
    assert(!direct_bms_parse_modbus(bms, sizeof(bms), 2, &bm));

    uint8_t private_query[7];
    assert(direct_bms_make_7c_query(private_query) == 7);
    const uint8_t arm_private_query[7] =
        {0x7c, 0x78, 0x24, 0x80, 0x01, 0x00, 0x00};
    assert(memcmp(private_query, arm_private_query,
                  sizeof(private_query)) == 0);
    uint8_t private_reply[23] = {0};
    private_reply[0] = 0x7c;
    private_reply[7] = 1;       /* one cell */
    private_reply[8] = 0xe4; private_reply[9] = 0x0c;
    private_reply[10] = 0;      /* no temperature entries */
    private_reply[11] = 0x9c; private_reply[12] = 0xff; /* -100 x 0.1 A */
    private_reply[13] = 0x08; private_reply[14] = 0x02; /* 52.0 V */
    private_reply[15] = 80;
    uint16_t private_crc = direct_crc16(private_reply + 3,
                                         sizeof(private_reply) - 3);
    private_reply[1] = (uint8_t)private_crc;
    private_reply[2] = (uint8_t)(private_crc >> 8);
    assert(direct_bms_parse_7c(private_reply, sizeof(private_reply), &bm));
    assert(bm.cell_mv[0] == 3300 && bm.current_10ma == -1000 &&
           bm.voltage_10mv == 5200 && bm.soc_percent == 80);
    private_reply[7] = 17;
    assert(!direct_bms_parse_7c(private_reply, sizeof(private_reply), &bm));

    const uint8_t handshake[8] = {0xff, 1, 2, 3, 4, 5, 6, 7};
    uint8_t boot_frame[15] = {0};
    assert(direct_bms_boot_make_frame(handshake, sizeof(handshake), boot_frame,
                                      sizeof(boot_frame)) == 15);
    assert(boot_frame[0] == 0x5a && boot_frame[1] == 0xff &&
           boot_frame[2] == 0x80 && boot_frame[14] == 0xa5);
    uint8_t reply[15] = {0x5a,0x80,0xff,0,8,0xff,1,2,0,0,0,0,0,0,0xa5};
    uint8_t sum = 0;
    for (int i = 1; i <= 12; ++i) sum = (uint8_t)(sum + reply[i]);
    reply[13] = (uint8_t)(0xffU - sum);
    direct_bms_boot_reply_t parsed;
    assert(direct_bms_boot_parse_reply(reply, sizeof(reply), 0xff, &parsed));
    assert(parsed.error == 0 && parsed.device_type == 1);
    reply[8] = 0xff;
    assert(!direct_bms_boot_parse_reply(reply, sizeof(reply), 0xff, &parsed));
    puts("direct protocol tests passed");
    return 0;
}
