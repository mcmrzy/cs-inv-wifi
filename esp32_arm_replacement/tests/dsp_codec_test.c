#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "direct_protocol.h"

/* Independent wire CRC, so fixtures do not borrow the encoder under test. */
static uint16_t wire_crc(const uint8_t *bytes, size_t len)
{
    uint16_t crc = 0xffffU;
    for (size_t i = 0; i < len; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (uint16_t)((crc >> 1) ^ ((crc & 1U) ? 0xa001U : 0U));
    }
    return crc;
}

static void finish_frame(uint8_t *frame, size_t len, uint8_t command)
{
    assert(len >= 5U && len <= DIRECT_DSP_MAX_FRAME);
    frame[0] = 1;
    frame[1] = (uint8_t)(len - 4U);
    frame[2] = command;
    uint16_t crc = wire_crc(frame, len - 2U);
    frame[len - 2U] = (uint8_t)crc;
    frame[len - 1U] = (uint8_t)(crc >> 8);
}

static void put_word(uint8_t *frame, size_t offset, int32_t value)
{
    uint16_t raw = (uint16_t)value;
    frame[offset] = (uint8_t)(raw >> 8);
    frame[offset + 1U] = (uint8_t)raw;
}

static void test_requests(void)
{
    const uint8_t status_query[9] = {1,5,3,0,0,0,0,0x4f,0x95};
    const uint8_t measurement_query[9] = {1,5,4,0,0,0,0,0xfa,0x55};
    uint8_t request[9];
    assert(direct_dsp_make_request(3, -1000, 5200, 80, request) == 9U);
    assert(memcmp(request, status_query, sizeof(request)) == 0);
    assert(direct_dsp_make_request(4, -1000, 5200, 80, request) == 9U);
    assert(memcmp(request, measurement_query, sizeof(request)) == 0);
    assert(direct_dsp_make_request(5, -1000, 5200, 80, request) == 9U);
    const uint8_t negative_current[7] = {1,5,5,0xff,0xff,0xfc,0x18};
    assert(memcmp(request, negative_current, sizeof(negative_current)) == 0);
    assert(wire_crc(request, 7) == (uint16_t)(request[7] | (request[8] << 8)));
    assert(direct_dsp_make_request(6, -1000, 5200, 80, request) == 9U);
    const uint8_t voltage_soc[7] = {1,5,6,0x14,0x50,0,80};
    assert(memcmp(request, voltage_soc, sizeof(voltage_soc)) == 0);
    assert(wire_crc(request, 7) == (uint16_t)(request[7] | (request[8] << 8)));
    assert(direct_dsp_validate(request, sizeof(request)));
    for (uint8_t cmd = 3; cmd <= 6; ++cmd) {
        assert(direct_dsp_make_request(cmd, 0, 0, 0, request) == 9U);
        for (unsigned i = 3; i <= 6; ++i) assert(request[i] == 0);
        assert(wire_crc(request, 7) == (uint16_t)(request[7] | (request[8] << 8)));
    }
    memset(request, 0x5a, sizeof(request));
    uint8_t before[9]; memcpy(before, request, sizeof(before));
    assert(direct_dsp_make_request(2, 0, 0, 0, request) == 0);
    assert(direct_dsp_make_request(7, 0, 0, 0, request) == 0);
    assert(memcmp(request, before, sizeof(request)) == 0);
    assert(direct_dsp_make_request(3, 0, 0, 0, NULL) == 0);
}

static void reject_status(const uint8_t *frame, size_t len)
{
    direct_dsp_status_t status;
    memset(&status, 0x5a, sizeof(status));
    uint8_t before[sizeof(status)]; memcpy(before, &status, sizeof(before));
    assert(!direct_dsp_parse_cmd03(frame, len, &status));
    assert(memcmp(before, &status, sizeof(status)) == 0);
}

static void test_status(void)
{
    uint8_t frame[22] = {0};
    frame[3]=0x80; frame[4]=0x12; frame[5]=0x34; frame[6]=0x56;
    put_word(frame,7,0x4081); put_word(frame,9,0x8123); put_word(frame,11,0xfedc);
    put_word(frame,13,0x1234); put_word(frame,15,0x5678); put_word(frame,17,0x9abc);
    frame[19]=16; finish_frame(frame,sizeof(frame),3);
    direct_dsp_status_t status;
    assert(direct_dsp_parse_cmd03(frame,sizeof(frame),&status));
    assert(status.fault==0x80123456U && status.mode==16);
    assert(status.inverter_status[0]==0x4081 && status.inverter_status[1]==0x8123 &&
           status.inverter_status[2]==0xfedc);
    assert(status.warning[0]==0x1234 && status.warning[1]==0x5678 &&
           status.warning[2]==0x9abc);
    uint8_t short_frame[21]={0}; finish_frame(short_frame,sizeof(short_frame),3);
    reject_status(short_frame,sizeof(short_frame));
    reject_status(frame,sizeof(frame)-1U);
    frame[1]++; reject_status(frame,sizeof(frame)); frame[1]--;
    frame[21]^=1; reject_status(frame,sizeof(frame)); frame[21]^=1;
    finish_frame(frame,sizeof(frame),4); reject_status(frame,sizeof(frame));
    memset(frame,0,sizeof(frame)); finish_frame(frame,sizeof(frame),3);
    assert(direct_dsp_parse_cmd03(frame,sizeof(frame),&status));
    assert(status.fault==0 && status.mode==0);
    for (unsigned i=0;i<3;++i)
        assert(status.inverter_status[i]==0 && status.warning[i]==0);
}

/* These are raw wire units, not a uniform engineering-unit conversion:
 * AC/PV volts 1 V, AC/PV/bus amps 0.01 A, frequencies/battery volts/amps/
 * temperature 0.1 units, W/VA 1 unit. Grid/load percent is 0.1%, inverter/
 * battery percent is 1%. Negative samples test signed wire decoding. */
static const int32_t samples[32] = {
    230,-1234,500,-1234,-1500,875,
    231,-2345,501,-2345,-2500,87,
    229,-3456,499,-3456,-3500,625,
    523,-125,-654,80,
    4012,-4567,
    350,1234,360,2345,4321,5432,9753,-123
};

static void measurement_fixture(uint8_t *frame, size_t len)
{
    memset(frame,0,len);
    for (unsigned i=0;i<32;++i) put_word(frame,3U+i*2U,samples[i]);
    if (len>69U) { frame[67]=0xa5; frame[68]=0x5a; }
    finish_frame(frame,len,4);
}

static void check_measurement(const direct_dsp_measurement_t *m)
{
    assert(m->grid_v==230 && m->grid_i==-1234 && m->grid_hz_tenths==500 &&
           m->grid_w==-1234 && m->grid_va==-1500 && m->grid_percent==875);
    assert(m->inverter_v==231 && m->inverter_i==-2345 && m->inverter_hz_tenths==501 &&
           m->inverter_w==-2345 && m->inverter_va==-2500 && m->inverter_percent==87);
    assert(m->load_v==229 && m->load_i==-3456 && m->load_hz_tenths==499 &&
           m->load_w==-3456 && m->load_va==-3500 && m->load_percent==625);
    assert(m->battery_v_tenths==523 && m->battery_i_tenths==-125 &&
           m->battery_w==-654 && m->battery_soc==80);
    assert(m->bus_v_tenths==4012 && m->bus_i==-4567);
    assert(m->pv1_v==350 && m->pv1_i==1234 && m->pv2_v==360 && m->pv2_i==2345 &&
           m->pv1_w==4321 && m->pv2_w==5432 && m->pv_total_w==9753);
    assert(m->inverter_temp_tenths==-123);
}

static void reject_measurement(const uint8_t *frame, size_t len)
{
    direct_dsp_measurement_t measurement;
    memset(&measurement,0x5a,sizeof(measurement));
    uint8_t before[sizeof(measurement)];
    memcpy(before,&measurement,sizeof(before));
    assert(!direct_dsp_parse_cmd04(frame,len,&measurement));
    assert(memcmp(before,&measurement,sizeof(before))==0);
}

static void test_measurement(void)
{
    uint8_t frame[69]; measurement_fixture(frame,sizeof(frame));
    direct_dsp_measurement_t measurement;
    assert(direct_dsp_parse_cmd04(frame,sizeof(frame),&measurement));
    check_measurement(&measurement);
    uint8_t short_frame[68]={0}; finish_frame(short_frame,sizeof(short_frame),4);
    reject_measurement(short_frame,sizeof(short_frame));
    reject_measurement(frame,sizeof(frame)-1U);
    frame[1]++; reject_measurement(frame,sizeof(frame)); frame[1]--;
    frame[68]^=1; reject_measurement(frame,sizeof(frame)); frame[68]^=1;
    finish_frame(frame,sizeof(frame),3); reject_measurement(frame,sizeof(frame));
    memset(frame,0,sizeof(frame)); finish_frame(frame,sizeof(frame),4);
    assert(direct_dsp_parse_cmd04(frame,sizeof(frame),&measurement));
    direct_dsp_measurement_t zero={0};
    assert(memcmp(&measurement,&zero,sizeof(zero))==0);
}

static void reject_config(const uint8_t *frame, size_t len)
{
    uint16_t values[DIRECT_DSP_CONFIG_COUNT],before[DIRECT_DSP_CONFIG_COUNT];
    bool valid[DIRECT_DSP_CONFIG_COUNT],valid_before[DIRECT_DSP_CONFIG_COUNT];
    for (unsigned i=0;i<DIRECT_DSP_CONFIG_COUNT;++i) {values[i]=0x5a5a; valid[i]=(i&1U)!=0;}
    memcpy(before,values,sizeof(before)); memcpy(valid_before,valid,sizeof(valid));
    assert(!direct_dsp_parse_cmd25(frame,len,values,valid));
    assert(memcmp(before,values,sizeof(values))==0);
    assert(memcmp(valid_before,valid,sizeof(valid))==0);
}

static void test_config(void)
{
    uint8_t frame[43]={0};
    frame[3]=4; frame[4]=3; frame[5]=1; frame[11]=80; frame[12]=1;
    put_word(frame,7,2300); put_word(frame,9,5000);
    put_word(frame,13,580); put_word(frame,15,560); put_word(frame,17,440);
    put_word(frame,19,480); put_word(frame,21,540);
    frame[23]=10; frame[24]=20; frame[25]=80; frame[26]=30;
    frame[27]=1; frame[28]=15; put_word(frame,29,590);
    put_word(frame,31,60); put_word(frame,33,120);
    frame[35]=1; frame[36]=2; frame[39]=1; frame[40]=9;
    finish_frame(frame,sizeof(frame),0x25);
    uint16_t values[DIRECT_DSP_CONFIG_COUNT]={0};
    bool valid[DIRECT_DSP_CONFIG_COUNT]={0};
    assert(direct_dsp_parse_cmd25(frame,sizeof(frame),values,valid));
    const uint16_t expected[][2]={
        {0,4},{13,3},{2,1},{1,80},{4,1},{40,10},{36,20},{37,80},
        {10,30},{26,1},{30,15},{32,1},{33,2},{31,1},{48,9},
        {7,2300},{8,5000},{11,480},{12,540},{27,590},{28,60},{29,120},
        {67,580},{70,560},{76,440}
    };
    for (unsigned i=0;i<sizeof(expected)/sizeof(expected[0]);++i)
        assert(valid[expected[i][0]] && values[expected[i][0]]==expected[i][1]);
    assert(!valid[5] && !valid[68] && !valid[71] && !valid[77]);
    uint8_t short_frame[42]={0}; finish_frame(short_frame,sizeof(short_frame),0x25);
    reject_config(short_frame,sizeof(short_frame));
    reject_config(frame,sizeof(frame)-1U);
    frame[1]++; reject_config(frame,sizeof(frame)); frame[1]--;
    frame[42]^=1; reject_config(frame,sizeof(frame)); frame[42]^=1;
    finish_frame(frame,sizeof(frame),4); reject_config(frame,sizeof(frame));
    memset(frame,0,sizeof(frame)); finish_frame(frame,sizeof(frame),0x25);
    assert(direct_dsp_parse_cmd25(frame,sizeof(frame),values,valid));
    for (unsigned i=0;i<22;++i) assert(valid[expected[i][0]] && values[expected[i][0]]==0);
    for (unsigned i=67;i<=81;++i) assert(!valid[i]);
}

int main(void)
{
    test_requests(); test_status(); test_measurement(); test_config();
    puts("PASS: DSP 03/04/25 boundaries, all 32 raw fields and signed values, legal zeros, 03/04/05/06 wire requests, rejected frames preserve outputs");
    fflush(stdout);
    uint8_t extended[71]; measurement_fixture(extended,sizeof(extended));
    direct_dsp_measurement_t measurement;
    if (!direct_dsp_parse_cmd04(extended,sizeof(extended),&measurement)) {
        fputs("FAIL: CRC-valid 71-byte DSP 04 extension must decode the known 32-field prefix\n",stderr);
        return 1;
    }
    check_measurement(&measurement);
    extended[70]^=1; reject_measurement(extended,sizeof(extended));
    puts("PASS: DSP 04 extension keeps known-prefix units and rejects invalid CRC");
    return 0;
}
