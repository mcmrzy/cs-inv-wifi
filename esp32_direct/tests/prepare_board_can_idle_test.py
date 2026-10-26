"""Exercise actual GPIO initialization and LCD writes with the board config."""
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


config = []
for line in (root / "sdkconfig").read_text(encoding="utf-8").splitlines():
    if not line.startswith("CONFIG_DIRECT_"):
        continue
    name, value = line.split("=", 1)
    config.append(f"#define {name} { {'y': '1', 'n': '0'}.get(value, value) }")
# Boolean options omitted by sdkconfig still need defined values in C expressions.
for name in ("DIRECT_LCD_CS_FIXED_LOW", "DIRECT_LCD_GPIO_SPI"):
    if not any(line.startswith(f"#define CONFIG_{name} ") for line in config):
        config.append(f"#define CONFIG_{name} 0")
(root / "tmp/board_can_config.inc").write_text("\n".join(config) + "\n")

functions = [extract(app, "direct_board_pins_valid")]
if "static void direct_can_idle_init(" in app:
    functions += ["#define HAVE_CAN_IDLE_INIT 1", extract(app, "direct_can_idle_init")]
functions += [extract(panel, "direct_panel_prepare_boot"), extract(panel, "tx"),
              extract(panel, "panel_register_init"), extract(panel, "panel_hardware_initialize"),
              extract(panel, "direct_panel_init")]
(root / "tmp/board_can_under_test.inc").write_text("\n".join(functions) + "\n")

