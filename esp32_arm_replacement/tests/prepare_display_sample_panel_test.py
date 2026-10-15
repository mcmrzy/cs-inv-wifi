from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / 'main/direct/direct_panel_runtime.inc').read_text(encoding='utf-8')
start = source.index('    if(arm_poll_take_param()){', source.index('static void panel_snapshot(void)'))
end = source.index('    direct_config_snapshot(', start)
(root / 'tmp/display_sample_panel_under_test.inc').write_text(
    'static void panel_snapshot_display_test(void) {\n' + source[start:end] + '}\n',
    encoding='utf-8',
)
