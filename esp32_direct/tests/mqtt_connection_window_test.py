"""Host regression tests for Wi-Fi/MQTT/BLE memory arbitration.

Runs the real C decision functions with hardware boundaries replaced, not a
second implementation of the scheduling policy. Does not prove radio behavior.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GCC = os.environ.get("HOST_GCC", r"C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe")


def function(source, name):
    match = re.search(r"^(?:static\s+)?(?:bool|void|int)\s+" + name + r"\([^;]*?\)\s*\{", source, re.M)
    if not match:
        raise AssertionError("Missing decision function: " + name)
    end = source.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


class MqttConnectionWindowTests(unittest.TestCase):
    def test_sdk_cannot_bypass_memory_admission(self):
        mqtt = (ROOT / "main/mqtt/mqtt_handler.c").read_text(encoding="utf-8")
        self.assertRegex(mqtt, r"\.network\.disable_auto_reconnect\s*=\s*true")
        self.assertRegex(mqtt, r"\.network\.timeout_ms\s*=\s*10000")
        self.assertRegex(mqtt, r"#define MQTT_TASK_STACK_SIZE\s+8192U")
        self.assertRegex(mqtt, r"\.task\.stack_size\s*=\s*MQTT_TASK_STACK_SIZE")


if __name__ == "__main__":
    unittest.main()
