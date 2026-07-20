# Firmware host-side regression checks

These checks require no ESP32 or ARM hardware and do not flash a device. They cover:

- UART escaping/checksum reference vectors and both firmware implementations
- numeric JSON `t` parsing and `task_id` duplicate suppression
- OTA status correlation/progress fields
- the ESP-IDF 4 MiB partition layout and SDK configuration

Run from the repository root:

```powershell
node --test tools/tests/firmware_contract.test.js
```

The source-contract checks are intentionally shared by the Arduino and ESP-IDF variants so
protocol fixes cannot silently drift between the two implementations.
