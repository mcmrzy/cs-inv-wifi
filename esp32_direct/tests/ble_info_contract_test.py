"""Execute the actual INFO encoder; only the ATT mbuf boundary is fake."""
import json
import unittest
import ble_ota_memory_lifecycle_test as lifecycle
ROOT = lifecycle.ROOT
function = lifecycle.function


class BleInfoContractTests(unittest.TestCase):
    compile_run = lifecycle.BleMemoryLifecycleTests.compile_run

    def test_info_current_and_longest_supported_version_fit(self):
        source = (ROOT / "components/ble_ct/ble_ct.c").read_text(encoding="utf-8")
        encoder = function(source, "info_access_cb")
        harness = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define BLE_GATT_ACCESS_OP_READ_CHR 1
#define BLE_ATT_ERR_UNLIKELY 14
#define BLE_ATT_ERR_INSUFFICIENT_RES 17
struct os_mbuf { char data[1024]; uint16_t len; };
struct ble_gatt_access_ctxt { int op; struct os_mbuf *om; };
static const char *s_device_sn="1234567890123456";
static const char *s_fw_version;
static char s_session_id[33]="0123456789abcdef0123456789abcdef";
static int os_mbuf_append(struct os_mbuf *om,const void *data,uint16_t len) {
    memcpy(om->data,data,len); om->data[len]=0; om->len=len; return 0;
}
ENCODER
int main(void) {
    struct os_mbuf om={0}; struct ble_gatt_access_ctxt ctxt={1,&om};
    const char *versions[]={"0.6.34-provisioning-dev","1234567890123456789012345678901"};
    for(unsigned i=0;i<2;i++) {
        s_fw_version=versions[i];
        assert(info_access_cb(7,1,&ctxt,NULL)==0);
        assert(om.len<=480);
        puts(om.data);
    }
    ctxt.op=2; assert(info_access_cb(7,1,&ctxt,NULL)==BLE_ATT_ERR_UNLIKELY);
}
'''.replace("ENCODER", encoder)
        # Execute through the shared real C compiler harness, retaining JSON output.
        import contextlib
        import io
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.compile_run(harness.replace("    if (ctxt->op", "    (void)conn_handle; (void)attr_handle; (void)arg;\n    if (ctxt->op", 1))
        lines = output.getvalue().splitlines()
        self.assertEqual(len(lines), 2)
        for line in lines:
            info = json.loads(line)
            body = info["body"]
            self.assertEqual(info["v"], 2)
            self.assertEqual(body["device_sn"], "1234567890123456")
            self.assertEqual(body["proto_version"], 2)
            self.assertTrue(body["bound"])
            self.assertEqual(body["max_message_bytes"], 512)
            self.assertNotIn("TBD_GATE", line)
            self.assertNotIn("system_controller", body.get("supported_upgrade_modules", []))
            self.assertTrue({"control", "ota", "ota_binary_v1", "framing_v1", "direct_control_v1"}.issubset(body["capabilities"]))


if __name__ == "__main__":
    unittest.main()
