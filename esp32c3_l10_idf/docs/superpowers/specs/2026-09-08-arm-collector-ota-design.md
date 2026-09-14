# ARM Collector OTA Design

## Goal

Upgrade the GD32 ARM application through ESP32 by using the Collector protocol already implemented by the ARM application and bootloader.

## Flow

1. ESP32 downloads and validates the raw `Inverter.bin` image into the cache partition.
2. ESP32 sends encrypted Collector command `0x07`, parameter 1, payload `ARMU`.
3. ARM application replies with status zero, then resets after the UART reply has drained.
4. ARM bootloader requests ARM firmware information with command `0x09`, parameter 1.
5. ESP32 replies with update flag zero, transport size, and the chained CRC of the encrypted transport payload.
6. ARM bootloader requests packet zero with `0x0A`. ESP32 sends command `0x0B` containing the packet id and encrypted firmware payload.
7. Packet zero contains a 16-byte transport header plus 1024 application bytes. Later packets contain at most 1024 application bytes. Each payload is independently encrypted with FwKey because the bootloader resets the CBC IV for each received packet.
8. Bootloader acknowledges each packet with `0x0B`; the final zero status completes the OTA.

## Safety and errors

Only a valid encrypted Collector control frame with parameter 1 and payload `ARMU` schedules the reset. The application sends an explicit status response before resetting. ESP32 pauses normal polling for the complete upgrade, retries timed-out control/data exchanges, rejects images that cannot be represented by the bootloader protocol, and uses the manifest timeout for the complete 9600-baud transfer.

## Verification

Host tests cross-check payload encryption, packet boundaries, chained CRC, and decrypt-to-original behavior. Source checks cover ARM control validation and delayed reset. ESP-IDF and ARM builds are run when their toolchains are available.
