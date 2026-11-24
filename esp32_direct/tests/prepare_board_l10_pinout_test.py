"""Exercise actual GPIO initialization and LCD writes with the new L10 defaults."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
app = (root / "main/app/app_main.c").read_text(encoding="utf-8")
panel = (root / "main/direct/direct_panel.c").read_text(encoding="utf-8")


def extract(source, name):
    start = source.index(name + "(")
    start = source.rfind("\n", 0, start) + 1
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


dflt = {}
for line in (root / "sdkconfig.defaults").read_text(encoding="utf-8").splitlines():
    if not line.startswith("CONFIG_DIRECT_"):
        continue
    name, value = line.split("=", 1)
    dflt[name] = {"y": "1", "n": "0"}.get(value, value)
for name, value in {
    "DIRECT_BMS_RS485_DE_PIN": -1, "DIRECT_LCD_CS_FIXED_LOW": 0,
    "DIRECT_LCD_GPIO_SPI": 0, "DIRECT_KEY_BS818A": 1,
    "DIRECT_KEY_ROW_UP_PIN": -1, "DIRECT_KEY_ROW_DOWN_PIN": -1,
    "DIRECT_KEY_ESC_PIN": -1, "DIRECT_KEY_ENTER_PIN": -1,
}.items():
    dflt.setdefault("CONFIG_" + name, value)
assert dflt["CONFIG_DIRECT_LCD_CS_PIN"] == "-1"
dflt["CONFIG_DIRECT_LCD_NOCS_BOOT"] = 1
config = [f"#define {name} {value}" for name, value in dflt.items()]
(root / "tmp/board_l10_config.inc").write_text("\n".join(config) + "\n")

functions = [extract(app, "direct_board_pins_valid"), extract(app, "direct_can_idle_init")]
functions += [extract(panel, "direct_panel_prepare_boot"), extract(panel, "tx"),
              extract(panel, "panel_register_init"), extract(panel, "panel_hardware_initialize"),
              extract(panel, "direct_panel_enable_display"),
              extract(panel, "direct_panel_init")]
(root / "tmp/board_l10_under_test.inc").write_text("\n".join(functions) + "\n")
