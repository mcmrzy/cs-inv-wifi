"""Compile the production framing codec and telemetry module with fake radio/OS."""
from pathlib import Path
import unittest
import ble_ota_memory_lifecycle_test as lifecycle

ROOT = lifecycle.ROOT


class BleFramesTests(unittest.TestCase):
    compile_run = lifecycle.BleMemoryLifecycleTests.compile_run

    def test_telemetry_mtu_cache_coherence_and_legacy_gate(self):
        telemetry = (ROOT / "components/ble_ct/ble_ct_telemetry.c").read_text(encoding="utf-8")
        # Compile every production function unchanged; replace includes with IDF boundaries.
        telemetry = "\n".join(line for line in telemetry.splitlines() if not line.startswith("#include"))
        codec = ROOT / "components/ble_ct/ble_ct_frames.c"
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "CODEC"
#define BLE_HS_CONN_HANDLE_NONE 65535
#define BLE_GATT_ACCESS_OP_READ_CHR 1
#define BLE_ATT_ERR_UNLIKELY 14
#define BLE_ATT_ERR_INSUFFICIENT_RES 17
#define BLE_ATT_ERR_INVALID_OFFSET 7
#define ESP_LOGW(tag,...) ((void)(tag))
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
static int snapshot_locked;
#define portENTER_CRITICAL(m) do { (void)(m); assert(!snapshot_locked); snapshot_locked=1; } while(0)
#define portEXIT_CRITICAL(m) do { (void)(m); assert(snapshot_locked); snapshot_locked=0; } while(0)
static bool gate,held,subscribed,framing,inject_update,append_fail;
static uint16_t conn=7,mtu=512;
static unsigned notified,notify_fail_at;
static uint8_t wire[1024]; static size_t wire_len;
struct os_mbuf { uint8_t data[1100]; uint16_t len; };
struct ble_gatt_access_ctxt { int op; struct os_mbuf *om; uint16_t offset; };
static struct os_mbuf pool[2]; static unsigned allocated;
void ble_ct_telemetry_set_snapshot(const char *,uint16_t);
static bool ble_prov_host_acquire(void) { if(!gate)return false; assert(!held); held=true; return true; }
static void ble_prov_host_release(void) { assert(held); held=false; }
static bool ble_ct_is_telemetry_subscribed(void) { return subscribed; }
static bool ble_ct_framing_enabled(void) { return framing; }
static uint16_t ble_ct_get_conn_handle(void) { return conn; }
static uint16_t ble_ct_get_telemetry_handle(void) { return 11; }
static uint16_t ble_att_mtu(uint16_t c) { assert(held && c==conn && !snapshot_locked); return mtu; }
static int os_mbuf_append(struct os_mbuf *m,const void *data,uint16_t len) {
    assert(!snapshot_locked); if(append_fail) return -1;
    memcpy(m->data+m->len,data,len/2);
    if(inject_update) { inject_update=false; ble_ct_telemetry_set_snapshot("new",3); }
    memcpy(m->data+m->len+len/2,(const uint8_t *)data+len/2,len-len/2);
    m->len+=len; return 0;
}
static struct os_mbuf *ble_hs_mbuf_from_flat(const void *data,uint16_t len) {
    assert(held && !snapshot_locked); struct os_mbuf *m=&pool[allocated++%2]; m->len=0;
    assert(os_mbuf_append(m,data,len)==0); return m;
}
static void os_mbuf_free_chain(struct os_mbuf *m) { (void)m; assert(!snapshot_locked); }
static void os_mbuf_concat(struct os_mbuf *m,struct os_mbuf *n) { assert(os_mbuf_append(m,n->data,n->len)==0); }
static int ble_gattc_notify_custom(uint16_t c,uint16_t h,struct os_mbuf *m) {
    assert(held && c==conn && h==11 && !snapshot_locked && m->len<=mtu-3);
    assert((m->data[0]&63)==notified);
    if(!notified) assert(m->data[0]&BLE_CT_FRAME_FIRST);
    if (notify_fail_at && notified + 1 == notify_fail_at) { notified++; return -1; }
    memcpy(wire+wire_len,m->data+1,m->len-1); wire_len+=m->len-1; notified++; return 0;
}
TELEMETRY
static void read_expect(const char *data,size_t len) {
    struct os_mbuf out={0}; struct ble_gatt_access_ctxt ctxt={1,&out,0};
    assert(!ble_ct_telemetry_read_cb(7,11,&ctxt,NULL));
    assert(out.len==len && !memcmp(out.data,data,len));
}
int main(void) {
    /* Fake OS mocks are radio boundaries, not implementation assertions. */
    (void)os_mbuf_free_chain; (void)os_mbuf_concat; (void)ble_ct_framing_enabled; (void)ble_att_mtu;
    read_expect("{}",2);
    ble_ct_telemetry_notify("offline",7); read_expect("offline",7); assert(!notified);
    gate=true; ble_ct_telemetry_notify("unsubscribed",12); read_expect("unsubscribed",12);
    subscribed=true; char data[1025]; memset(data,'x',sizeof(data));
    const uint16_t mtus[]={23,185,247,256,512};
    framing=true;
    for(unsigned i=0;i<5;i++) {
        mtu=mtus[i]; notified=0; wire_len=0;
        ble_ct_telemetry_notify(data,1024);
        assert(notified==(1024u+mtu-5u)/(mtu-4u));
        assert(wire_len==1024 && !memcmp(wire,data,1024) && !held);
    }
    notified=0; wire_len=0; mtu=23; notify_fail_at=2;
    ble_ct_telemetry_notify(data,1024); assert(notified==2 && !held);
    notify_fail_at=0;
    notified=0; wire_len=0; mtu=23; framing=false;
    ble_ct_telemetry_notify(data,200); assert(!notified); read_expect(data,200);
    ble_ct_telemetry_notify(data,152); assert(notified==8); read_expect(data,152);
    ble_ct_telemetry_notify(data,1025); assert(notified==8); read_expect(data,152);
    ble_ct_telemetry_set_snapshot("old-snapshot",12); inject_update=true;
    read_expect("old-snapshot",12); read_expect("new",3);
    char original[200],updated[200]; memset(original,'a',200); memset(updated,'b',200);
    ble_ct_telemetry_set_snapshot(original,200);
    struct os_mbuf blob={0}; struct ble_gatt_access_ctxt blob_ctxt={1,&blob,0};
    assert(!ble_ct_telemetry_read_cb(7,11,&blob_ctxt,NULL));
    ble_ct_telemetry_set_snapshot(updated,200);
    for(unsigned offset=22;offset<=200;offset+=22) {
        blob.len=0; blob_ctxt.offset=offset;
        assert(!ble_ct_telemetry_read_cb(7,11,&blob_ctxt,NULL));
        assert(blob.len==200 && !memcmp(blob.data,original,200));
    }
    read_expect(updated,200);
    blob_ctxt.offset=200; blob.len=0;
    assert(!ble_ct_telemetry_read_cb(7,11,&blob_ctxt,NULL));
    blob_ctxt.offset=201;
    assert(ble_ct_telemetry_read_cb(7,11,&blob_ctxt,NULL)==BLE_ATT_ERR_INVALID_OFFSET);
    ble_ct_telemetry_reset_connection(); blob_ctxt.offset=22;
    assert(ble_ct_telemetry_read_cb(7,11,&blob_ctxt,NULL)==BLE_ATT_ERR_INVALID_OFFSET);
    conn=BLE_HS_CONN_HANDLE_NONE; ble_ct_telemetry_notify("disconnected",12);
    read_expect("disconnected",12); assert(!held);
    struct os_mbuf out={0}; struct ble_gatt_access_ctxt ctxt={1,&out,0}; append_fail=true;
    assert(ble_ct_telemetry_read_cb(7,11,&ctxt,NULL)==BLE_ATT_ERR_INSUFFICIENT_RES);
    puts("PASS: actual telemetry MTU sizes, offline cache, 1024 bounds, coherent reads and legacy 8-frame gate");
}
'''.replace("CODEC", codec.as_posix()).replace("TELEMETRY", telemetry)
        self.compile_run(harness)

    def test_codec_roundtrip_boundaries_and_strict_reassembly(self):
        codec = ROOT / "components/ble_ct/ble_ct_frames.c"
        self.assertTrue(codec.exists(), "Reusable bounded framing codec is missing")
        harness = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "CODEC"
int main(void) {
    const uint16_t mtus[]={23,185,247,256,512};
    uint8_t message[1024],frame[512];
    for(size_t i=0;i<sizeof(message);i++) message[i]=(uint8_t)i;
    for(size_t m=0;m<5;m++) for(size_t len=1;len<=1024;len+=len<10?1:97) {
        ble_ct_frame_rx_t rx={0}; size_t offset=0,frame_len=0; uint8_t seq=0;
        while(offset<len) {
            int sent=ble_ct_frames_next(message,len,mtus[m],&offset,&seq,frame,sizeof(frame),&frame_len);
            assert(sent>=0 && frame_len<=mtus[m]-3u);
            int received=ble_ct_frames_accept(&rx,frame,frame_len,100+seq,1000,1024);
            assert(received==sent);
        }
        assert(!rx.active && rx.len==len && !memcmp(rx.data,message,len));
    }
    ble_ct_frame_rx_t rx={0};
    const uint8_t first[]={0x80,'a'},next[]={0x41,'b'},single[]={0xC0,'c'},badfirst[]={0x81,'x'};
    assert(ble_ct_frames_accept(&rx,next,2,0,100,512)<0);
    assert(ble_ct_frames_accept(&rx,badfirst,2,0,100,512)<0);
    assert(ble_ct_frames_accept(&rx,first,1,0,100,512)<0);
    assert(ble_ct_frames_accept(&rx,first,2,0,100,512)==0);
    assert(ble_ct_frames_accept(&rx,next,2,101,100,512)<0 && !rx.active);
    assert(ble_ct_frames_accept(&rx,first,2,200,100,512)==0);
    assert(ble_ct_frames_accept(&rx,first,2,201,100,512)<0 && !rx.active);
    assert(ble_ct_frames_accept(&rx,first,2,200,100,512)==0);
    assert(ble_ct_frames_accept(&rx,next,2,201,100,1)<0 && !rx.active);
    assert(ble_ct_frames_accept(&rx,single,2,202,100,512)==1 && rx.len==1);
    /* Unsigned elapsed time supports the monotonic millisecond counter wrapping. */
    assert(ble_ct_frames_accept(&rx,first,2,UINT32_MAX-5,100,512)==0);
    assert(ble_ct_frames_accept(&rx,next,2,5,100,512)==1 && rx.len==2);
    assert(ble_ct_frames_accept(&rx,first,2,0,100,1024)==0);
    for(unsigned i=1;i<63;i++) { uint8_t part[]={(uint8_t)i,'a'}; assert(ble_ct_frames_accept(&rx,part,2,i,100,1024)==0); }
    uint8_t overflow_seq[]={63,'b'};
    assert(ble_ct_frames_accept(&rx,overflow_seq,2,63,100,1024)<0 && !rx.active);
    assert(ble_ct_frames_accept(&rx,first,2,0,100,1024)==0);
    for(unsigned i=1;i<63;i++) { uint8_t part[]={(uint8_t)i,'a'}; assert(ble_ct_frames_accept(&rx,part,2,i,100,1024)==0); }
    const uint8_t final64[]={0x7f,'b'};
    assert(ble_ct_frames_accept(&rx,final64,2,63,100,1024)==1 && rx.len==64);
    uint8_t big[514]={0xC0};
    assert(ble_ct_frames_accept(&rx,big,514,1,100,512)<0);
    size_t offset=0,frame_len=0; uint8_t seq=0;
    assert(ble_ct_frames_next(message,1025,23,&offset,&seq,frame,512,&frame_len)<0);
    assert(ble_ct_frames_next(message,10,4,&offset,&seq,frame,512,&frame_len)<0);
    assert(ble_ct_frames_next(message,10,23,&offset,&seq,frame,1,&frame_len)<0);
    assert(ble_ct_frame_payload_capacity(23)==19 && ble_ct_frame_payload_capacity(512)==508);
    ble_ct_frames_reset(&rx); assert(!rx.len && !rx.active && !rx.next_seq);
    puts("PASS: actual codec MTU 23/185/247/256/512, malformed, order, timeout, bounds and wrap");
}
'''.replace("CODEC", codec.as_posix())
        self.compile_run(harness)


if __name__ == "__main__":
    unittest.main()
