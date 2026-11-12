"""Guard the full integration against accidentally restoring the private DSP bus."""
from pathlib import Path
import re

source = (Path(__file__).resolve().parents[1] / 'full/direct_poll.c').read_text(encoding='utf-8')
for forbidden in ['DSP_PORT', 'DSP_BAUD', 'direct_dsp_make_request(', 'direct_dsp_params_bootstrap(',
                  'direct_dsp_ota_flash(', 'direct_dsp_params_store_save(']:
    assert forbidden not in source, forbidden
for match in re.finditer(r'\buart_(?:write_bytes|read_bytes|flush_input|wait_tx_done|param_config|set_pin|driver_install)\(([^,\)]+)', source):
    assert match.group(1).strip() == 'BMS_PORT', match.group(0)
assert 'image->target!=DIRECT_IAP_BMS' in source
assert 'panel_bus_submit_sequence' in source
assert 'panel_result_find(&s_mb,s_write.sequence)' in source
assert 'PANEL_WRITE_PROCESSED' in source
assert 'panel_bus_set_quiet' in source
assert 'panel_bus_software_healthy' in source
assert 'direct_bms_encode_summary_7c' in source
assert 'direct_iap_run_legacy' in source
assert 's_write.sequence' in source
assert 'if(notify)' not in source  # callbacks use a copied list after releasing the parameter lock
print('PASS full poll source: one Modbus owner, BMS/IAP retained, no private DSP replay, exact receipt guard')
root=Path(__file__).resolve().parents[1]
boot=(root/'full/app_main.c').read_text(encoding='utf-8-sig')
assert 'esp_log_level_set("panel_bus", ESP_LOG_INFO)' in boot
assert 'esp_log_level_set("modbus_direct", ESP_LOG_INFO)' in boot
assert 'g_arm_stat.last_rx_ms=panel_stamp(&s_mb,0x42)' in source
assert 'INV_DATA %s problem_reg=' in source
assert 'UART1 TX=%d RX=%d bytes=%u failures=%lu head=%s' in source
bus=(root/'main/panel_bus.c').read_text(encoding='utf-8-sig')
assert 'INV_LINK %s UART0 TX=1 RX=0 invert=off' in bus
assert 'INV_RX last_fail addr=' in bus
bridge=source[source.index('static void bridge_task('):source.index('/* BMS IAP callbacks')]
assert 'ESP_LOG' not in bridge and 'printf(' not in bridge, 'Small bridge task must not enter printf/newlib'
tick=source[source.index('void arm_poll_tick('):source.index('void arm_poll_set_quiet(')]
assert tick.index('arm_poll_give_param();') < tick.index('log_bridge_diagnostics(')
assert 'uart_set_pin(UART_NUM_0,1,0,' in bus, 'Hardware-proven UART0 TX1/RX0'
assert 'uart_set_line_inverse(UART_NUM_0,0)' in bus
for rel in ['sdkconfig.full','full/sdkconfig.defaults']:
    config=(root/rel).read_text(encoding='utf-8-sig')
    for name,value in [('DIRECT_DSP_TX_PIN',1),('DIRECT_DSP_RX_PIN',0),
                       ('CS_INV_UART_TX_PIN',1),('CS_INV_UART_RX_PIN',0),
                       ('DIRECT_BMS_TX_PIN',21),('DIRECT_BMS_RX_PIN',20)]:
        assert re.search(r'^CONFIG_'+name+'='+str(value)+r'$',config,re.M),(rel,name)
assert 'bridge_stack_free=%u' in source
assert 'diagnostic=s_data_diagnostic;' in tick
assert 'Modbus: TX1 RX0 INV:both' not in (root/'full/direct_ui.c').read_text(encoding='utf-8')
assert '"GPIO1 TX / GPIO0 RX"' in (root/'main/panel_ui.c').read_text(encoding='utf-8')
assert '#define MAIN_LOOP_STACK_SIZE 6144U' in boot
assert 'arm_poll_tick();' in boot
print('PASS diagnostic wiring: visible at normal log level, UART0 status reachability distinct from full data, bounded UART1 hex')
print('PASS hotfix: bridge avoids newlib formatting, log after unlock on main loop, pin swap/config/help consistent, BMS pins unchanged')


