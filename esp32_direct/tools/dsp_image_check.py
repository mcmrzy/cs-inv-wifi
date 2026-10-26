"""Pre-flight check for an F280039C DSP OTA image, before publishing it.

Checks the file against the packed-image layout described in
docs/F280039C_ARM_GUIDE.html and docs/esp32/README.md, and against the header
that the two known-good releases (Invt280039C2_1.1.50V.bin / _1.1.400V.bin)
actually use:

  words 0..1   App entry LB: (w0 & 0xFFFC) == 0x0048, w1 = target low word
  words 2..3   magic 0x5A5A 0xA5A5
  words 4..5   payload length in 16-bit words, big endian
  words 6..7   zlib CRC32 of the payload, big endian
  words 8..9   first two payload words, written but outside length/CRC
  words 10..   payload covered by length and CRC32

The layout above was derived from both released images and reproduces their
length and CRC32 exactly; it is still not a substitute for the DSP boot's own
magic/length/CRC32 check and a real device acceptance run. No hardware access.
"""
from pathlib import Path
import argparse
import sys
import zlib

MAX_IMAGE_BYTES = 367 * 1024
APP_BASE_WORD = 0x082000
HEADER_WORDS = 8


def words(data: bytes, count: int, offset: int = 0) -> list[int]:
    return [int.from_bytes(data[offset + 2 * i:offset + 2 * i + 2], "big")
            for i in range(count)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", help="packed .bin produced by host/pack_f28003x.py")
    parser.add_argument("--expect-size", type=int, default=None,
                        help="also require this exact byte length")
    args = parser.parse_args()

    path = Path(args.image)
    if not path.is_file():
        print(f"FAIL  file not found: {path}")
        return 2
    data = path.read_bytes()
    reasons: list[str] = []

    print(f"file      : {path}")
    print(f"size      : {len(data)} bytes ({len(data) / 1024:.1f} KiB, "
          f"{(len(data) + 1023) // 1024} packets)")
    print(f"first 32B : {' '.join(f'{b:02x}' for b in data[:32])}")
    head = words(data, 8)
    print("header words (offset 0..7, big endian): " +
          " ".join(f"{w:04x}" for w in head))

    if len(data) < 64:
        reasons.append(f"IMAGE_SIZE_INVALID: {len(data)} bytes < 64")
    if len(data) % 2:
        reasons.append(f"IMAGE_SIZE_INVALID: odd length {len(data)} "
                       "(Intel HEX text is a common cause)")
    if len(data) > MAX_IMAGE_BYTES:
        reasons.append(f"IMAGE_SIZE_INVALID: {len(data)} > {MAX_IMAGE_BYTES}")
    if args.expect_size is not None and len(data) != args.expect_size:
        reasons.append(f"IMAGE_SIZE_INVALID: expected {args.expect_size}")
    if (head[0] & 0xFFFC) != 0x0048:
        reasons.append(f"IMAGE_LB_OPCODE_INVALID: first word {head[0]:#06x}")
    if data[4:6] != b"\x5a\x5a":
        reasons.append(f"IMAGE_MAGIC_INVALID: bytes 4..5 are {data[4:6].hex(' ')} "
                       "(run host/pack_f28003x.py on app.out first)")

    length_words = int.from_bytes(data[8:12], "big")
    stored_crc = int.from_bytes(data[12:16], "big")
    payload = data[20:]
    payload_words = len(payload) // 2
    computed_crc = zlib.crc32(payload) & 0xFFFFFFFF
    if data[6:8] != b"\xa5\xa5":
        reasons.append(f"IMAGE_MAGIC_INVALID: bytes 6..7 are {data[6:8].hex(' ')} "
                       "(expected a5 a5)")
    if length_words != payload_words:
        reasons.append(f"IMAGE_LENGTH_FIELD_MISMATCH: header {length_words} words, "
                       f"payload from offset 20 is {payload_words} words")
    if stored_crc != computed_crc:
        reasons.append(f"IMAGE_CRC_MISMATCH: header {stored_crc:#010x}, "
                       f"zlib(payload) {computed_crc:#010x}")

    print(f"entry LB  : {head[0]:#06x} -> {(head[0] & 0xFFFC) == 0x0048}")
    print(f"magic     : {data[4:6].hex(' ')} -> {data[4:6] == bytes([0x5A, 0x5A])}")
    print(f"app base  : 0x{APP_BASE_WORD:06X} (word address)")
    print(f"length    : header {length_words} words, payload {payload_words} words "
          f"-> {length_words == payload_words}")
    print(f"crc32     : header {stored_crc:#010x}, zlib(payload) {computed_crc:#010x} "
          f"-> {stored_crc == computed_crc}")

    if reasons:
        for reason in reasons:
            print(f"FAIL  {reason}")
        print("\nThe ESP32 will refuse this file before CMD42: "
              "no reset, no erase, no data frames.")
        return 1
    print("\nOK    length, entry LB opcode, header magic, payload length and CRC32 "
          "are all consistent.")
    print("      The DSP boot's own header check and the handoff still need device "
          "acceptance.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
