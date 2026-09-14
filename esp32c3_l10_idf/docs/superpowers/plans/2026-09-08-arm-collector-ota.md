# ARM Collector OTA Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the incompatible ESP32 ARM UART updater with the Collector bootloader protocol and add a reliable application-to-boot transition.

**Architecture:** A pure C codec prepares and validates bootloader payloads. The existing ESP32 UART polling task owns Collector framing and dispatches OTA frames to the OTA state machine. ARM App acknowledges an authenticated control command and resets on its periodic task; ARM Boot uses deterministic chained CRC.

**Tech Stack:** C, ESP-IDF, GD32F303 firmware, host GCC/Python tests

---

## Chunk 1: Protocol codec

- [ ] Add failing host tests for packet layout, encryption round trip, and chained CRC.
- [ ] Implement the pure C ARM IAP codec.
- [ ] Run host tests and existing Collector protocol tests.

## Chunk 2: ARM firmware changes

- [ ] Add source-level failing checks for control command validation and delayed reset.
- [ ] Implement `CTRLCMD/ARMU` acknowledgement and delayed reset in ARM App.
- [ ] Replace the broken Boot CRC function with the known-good chained software implementation.

## Chunk 3: ESP32 integration

- [ ] Rewrite ARM OTA around commands `0x07`, `0x09`, `0x0A`, and `0x0B`.
- [ ] Route Collector frames through the existing PPP/TEA parser during OTA.
- [ ] Replace the fixed ten-second install deadline with the manifest deadline.
- [ ] Run host tests, ESP-IDF build, ARM build where available, and inspect scoped diffs.
