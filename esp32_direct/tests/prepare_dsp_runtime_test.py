"""Extract real DSP task, frame application and freshness for UART transcripts."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
source = (root / "main/direct/direct_poll.c").read_text(encoding="utf-8")


def function(name: str) -> str:
    match = re.search(r"^(?:static )?(?:bool|void|uint16_t|size_t|int) " + name + r"\([^;]*?\)\s*\{", source, re.M)
    if not match:
        raise RuntimeError(f"Production function missing: {name}")
    brace = source.index("{", match.start())
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


names = ["queue_user_write", "collector_value", "collector_to_dsp", "arm_poll_take_param", "arm_poll_give_param",
         "direct_battery_config_get", "direct_battery_config_profiles", "direct_battery_config_write",
         "direct_dsp_get_measurement", "direct_dsp_get_diagnostics",
         "arm_poll_write_pending", "direct_config_write",
         "arm_poll_send_write", "direct_config_get", "direct_config_get_wire", "direct_config_observed_snapshot_wire", "direct_config_snapshot", "direct_config_writable", "arm_poll_request_config",
         "direct_config_reset_local",
         "apply_dsp04", "handle_dsp_frame", "dsp_accept_frame",
         "dsp_dump_capture", "dsp_uart_event_tick", "direct_dsp_params_install", "direct_dsp_params_export",
         "dsp_params_persist_tick", "dsp_report", "dsp_dispatch_notifications", "dsp_report_tick",
         "dsp_note_tx", "dsp_task", "arm_poll_tick"]
period = "\n".join(re.findall(
    r"^#define DSP_(?:POLL_INTERVAL_MS|MEASUREMENT_POLL_MS) .+$", source, re.M))
if "DSP_POLL_INTERVAL_MS" not in period or "DSP_MEASUREMENT_POLL_MS" not in period:
    raise RuntimeError("DSP poll period missing")
period = re.match(r".*", period, re.S)   # 保持与旧用法一致的 group(0)
header = (root / "main/direct/direct_link.h").read_text(encoding="utf-8")
feature = "#define DSP_RUNTIME_HAS_RX_ERRORS 1\n" if re.search(r"\brx_errors\b", header) else ""
feature += "#define DSP_UART_EVENT_QUEUE_LENGTH 16\n"
for name in ("dsp_task", "dsp_uart_event_tick", "handle_dsp_frame", "dsp_accept_frame", "apply_dsp04"):
    owner_code = function(name)
    for forbidden in ("ESP_LOG", "snprintf", "dsp_report(", "s_write_cb[", "s_ctrl_cb("):
        if forbidden in owner_code:
            raise RuntimeError(f"DSP UART owner still calls formatting/callback: {name}: {forbidden}")
# 整段 owner 侧代码(轮询 + 直连升级收发)都不许格式化或打日志: newlib 的
# vfprintf/锁路径要 1KB+ 栈, 4096B 的 direct_dsp 任务会在升级路径上栈溢出
# (0.6.8 与 0.6.21 两次实机崩溃的共同原因)。升级失败证据只记录, 由
# dsp_report_tick 在 6144B 的 main_loop 里打印。
owner_region = source[source.index("static uint32_t now_ms(void)"):source.index("/* Shared enqueue")]
if "ESP_LOG" in owner_region:
    raise RuntimeError("DSP/BMS UART owner region must not log from the owner task: ESP_LOG")
for call in re.findall(r"\b(?:snprintf|vsnprintf|fprintf|printf|puts)\s*\(", owner_region):
    raise RuntimeError(
        "DSP/BMS UART owner region must not format from the owner task: " + call)
(root / "tmp/dsp_runtime_under_test.inc").write_text(
    feature + period.group(0) + "\n\n" + "\n\n".join(function(name) for name in names) + "\n",
    encoding="utf-8",
)
