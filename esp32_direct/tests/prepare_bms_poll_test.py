"""Extract the production BMS task for a deterministic UART/timer boundary test."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
source = (root / "main/direct/direct_poll.c").read_text(encoding="utf-8")
start = source.index("static void bms_task(")
brace = source.index("{", start)
end, depth = brace + 1, 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
period = re.search(r"^#define BMS_POLL_INTERVAL_MS .+$", source, re.M)
(root / "tmp/bms_poll_under_test.inc").write_text(
    (period.group(0) + "\n" if period else "") + source[start:end] + "\n",
    encoding="utf-8",
)
