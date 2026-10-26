"""Verify the packaged GPIO4/LCD fix; this never connects to hardware."""
from pathlib import Path
import hashlib
import json
import shutil

root = Path(__file__).resolve().parents[1]
version = "0.6.4-can-idle-high"
release = root / "release/esp32c3_v0.6.4_can_idle_high_20261013/device"
description = json.loads((root / "build/project_description.json").read_text())
assert description["project_version"] == version
assert description["target"] == "esp32c3"
assert Path(description["config_file"]).name == "sdkconfig.device"
config = (root / "build/config/sdkconfig.h").read_text()
for marker in (
    "#define CONFIG_DIRECT_CAN_IDLE_TX_PIN 4",
    "#define CONFIG_DIRECT_LCD_CS_PIN -1",
    "#define CONFIG_DIRECT_LCD_NOCS_BOOT 1",
    "#define CONFIG_DIRECT_BMS_TX_PIN 21",
    "#define CONFIG_DIRECT_BMS_RX_PIN 20",
    "#define CONFIG_DIRECT_BMS_RS485_DE_PIN -1",
    "#define CONFIG_BOOT_ROM_LOG_ALWAYS_OFF 1",
    "#define CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG 1",
):
    assert marker in config, marker
assert "#define CONFIG_DIRECT_LCD_CS_FIXED_LOW 1" not in config
app = (release / "app.bin").read_bytes()
assert app == (root / "build/cs_inv_direct_c3.bin").read_bytes()
assert b"CAN_IDLE_HIGH: GPIO%d; LCD CS external low" in app
assert b"LCD CS_FIXED_LOW:" not in app
assert b"CS: GPIO4 held low" not in app
assert version.encode() in app
merged = release / "cs_inv_direct_c3_v0.6.4_device_4MB.bin"
image = merged.read_bytes()
assert len(image) == 4194304
flash = json.loads((root / "build/flasher_args.json").read_text())
end = 0
for offset, filename in sorted(flash["flash_files"].items(), key=lambda p: int(p[0], 0)):
    address = int(offset, 0)
    segment = (root / "build" / filename).read_bytes()
    assert image[end:address] == b"\xff" * (address - end)
    assert image[address:address + len(segment)] == segment
    end = address + len(segment)
assert image[end:] == b"\xff" * (len(image) - end)
manifest_path = release / "flash_manifest.json"
manifest = json.loads(manifest_path.read_text())
assert manifest["sha256"][merged.name] == hashlib.sha256(image).hexdigest()
manifest.update({"flash_address": "0x000000", "size": len(image),
                 "gpio4": "CAN D held high from application entry",
                 "lcd_cs": "independently tied low in hardware; no GPIO owner",
                 "overwrites_nvs_and_factory_partition": True,
                 "hardware_flashed": False, "bms_beep_hardware_verified": False})
manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
shutil.copyfile(root / "test_v064_can_idle_high.log", release / "tests.log")
shutil.copyfile(root / "docs/BMS_PROTOCOL_FROM_SOURCE_AND_ESP_HARDWARE.md",
                release / "BMS_PROTOCOL_FROM_SOURCE_AND_ESP_HARDWARE.md")
(release / "README.txt").write_text(
    "版本：0.6.4-can-idle-high，ESP32-C3，Device 配置。\n"
    "4MB 合并镜像：cs_inv_direct_c3_v0.6.4_device_4MB.bin\n"
    "烧录起始地址：0x000000；DIO / 80MHz / 4MB。\n"
    "适用接法：屏幕 NSS 独立硬件下拉，不再与 GPIO4 相连；GPIO4 本身不能接地。\n"
    "GPIO4 在应用入口固定高，释放 U30 CAN 总线；屏幕 CS=-1，继续显示。\n"
    "RS485 帧、波特率、500ms 轮询、原 UART ROM 静默设置保持。\n"
    "4MB 全量烧录会清除原 NVS、Wi-Fi 配网、缓存及 factory 数据。\n"
    "只编译和验证了本地文件；未烧录，BMS 四声是否消失需实机复测。\n"
    "应用运行前的 ROM/复位阶段 GPIO4 电平不由本修复保证。\n",
    encoding="utf-8")
print(f"PASS release: final GPIO ownership/config/profile, 4MB segments/FF/SHA256; {merged}")
print(f"SHA256 {hashlib.sha256(image).hexdigest()}")
