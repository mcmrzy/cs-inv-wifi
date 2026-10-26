# 0.6.18 display retention

0.6.17 still replaced LCD readings with -- after a 5-second receive gap.
This version keeps the most recent real 04 measurement and 03 status for display,
labelled Data stale / 数据过期. A new valid sample replaces the cached value.
No sample since startup, or explicit OTA/Boot transition, still shows unknown.
No periodic repaint: unchanged visible data and increasing age do not redraw.
DSP online and alarm validity remain separate from cached display availability.
Cloud telemetry, control, energy integration, UART polling and parameter sync are unchanged.
This repairs display overwrite; it does not prove the cause of missing DSP replies.
Flash app.bin to preserve identity, WiFi and parameters. Hardware acceptance pending.
