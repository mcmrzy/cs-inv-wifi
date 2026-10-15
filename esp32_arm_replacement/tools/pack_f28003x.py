"""Pack an F280039C App image into the DSP OTA ``.bin`` the boot expects.

Equivalent to the DSP boot project's ``host/pack_f28003x.py`` for the layout the
two released images (``Invt280039C2_1.1.50V.bin`` / ``_1.1.400V.bin``) use:

  word 0..1   App entry LB (taken verbatim from the App image)
  word 2..3   magic 0x5A5A 0xA5A5
  word 4..5   payload length in words, big endian (words 10..end)
  word 6..7   zlib CRC32 of the payload, big endian
  word 8..9   first two payload words (App code; outside length/CRC)
  word 10..   payload

Input is either a CCS Intel HEX (word-addressed, the format ``hex2000 -i
-memwidth 16 -romwidth 16`` writes) or a raw word image. Words 2..7 of the App
image must be unused (absent in the HEX, i.e. reserved by the App linker); the
packer fills them with the header. ``tools/dsp_image_check.py`` verifies the
result. This tool never touches hardware.
"""
from pathlib import Path
import argparse
import sys
import zlib

APP_BASE_WORD = 0x082000
APP_LIMIT_WORD = 0x0AFEF0        # App area 0x082000..0x0AFEEF per the ARM guide
MAX_PAYLOAD_WORDS = 367 * 512
MAGIC = bytes([0x5A, 0x5A, 0xA5, 0xA5])
HEADER_END_WORD = APP_BASE_WORD + 8
CRC_START_WORD = APP_BASE_WORD + 10


def read_hex_words(path: Path) -> dict[int, int]:
    """Return {word address: word} from a word-addressed Intel HEX file."""
    words: dict[int, int] = {}
    base = 0
    seen_eof = False
    for number, line in enumerate(path.read_text(encoding="ascii").splitlines(), 1):
        if not line.strip():
            continue
        if not line.startswith(":"):
            raise SystemExit(f"{path}:{number}: not an Intel HEX record")
        record = bytes.fromhex(line[1:])
        if len(record) != record[0] + 5 or sum(record) % 256:
            raise SystemExit(f"{path}:{number}: bad record length/checksum")
        address, kind = int.from_bytes(record[1:3], "big"), record[3]
        payload = record[4:-1]
        if kind == 4:
            base = int.from_bytes(payload, "big") << 16
        elif kind == 0:
            if len(payload) % 2:
                raise SystemExit(f"{path}:{number}: odd data length")
            word = base + address
            for i in range(0, len(payload), 2):
                words[word + i // 2] = int.from_bytes(payload[i:i + 2], "big")
        elif kind == 1:
            seen_eof = True
            break
        else:
            raise SystemExit(f"{path}:{number}: unsupported record type {kind}")
    if not seen_eof:
        raise SystemExit(f"{path}: missing EOF record")
    return words


def read_raw_words(path: Path, base: int) -> dict[int, int]:
    data = path.read_bytes()
    if len(data) % 2:
        raise SystemExit(f"{path}: {len(data)} bytes is not a whole number of words")
    return {base + i // 2: int.from_bytes(data[i:i + 2], "big")
            for i in range(0, len(data), 2)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", help="CCS Intel HEX or raw word image")
    parser.add_argument("output", help="packed .bin to write")
    parser.add_argument("--raw-base-word", type=int, default=APP_BASE_WORD,
                        help="first word address of a raw input image")
    args = parser.parse_args()

    source, target = Path(args.input), Path(args.output)
    if not source.is_file():
        print(f"FAIL  input not found: {source}")
        return 2
    if source.resolve() == target.resolve():
        print("FAIL  output must differ from input")
        return 2

    words = (read_raw_words(source, args.raw_base_word)
             if source.suffix.lower() == ".bin" else read_hex_words(source))
    if not words:
        print("FAIL  no data words found")
        return 1

    first = min(words)
    reserved = [first + 2, first + 3, first + 4, first + 5, first + 6, first + 7]
    occupied = [w for w in reserved if words.get(w, 0xFFFF) != 0xFFFF]
    if occupied:
        print(f"FAIL  App image uses the reserved header words "
              f"{' '.join(f'{w:06X}' for w in occupied)}; the App must be linked "
              f"with words {first + 2:06X}..{first + 7:06X} free")
        return 1
    if first != APP_BASE_WORD:
        print(f"FAIL  App base is 0x{first:06X}, expected 0x{APP_BASE_WORD:06X}")
        return 1

    last = max(words)
    if words[first] & 0xFFFC != 0x0048:
        print(f"FAIL  word 0 is 0x{words[first]:04x}, not an App entry LB opcode")
        return 1
    if last >= APP_LIMIT_WORD:
        print(f"FAIL  App ends at word 0x{last:06X}, outside the App area "
              f"0x{APP_BASE_WORD:06X}..0x{APP_LIMIT_WORD - 1:06X}")
        return 1

    image = bytearray()
    for word in range(first, last + 1):
        image += words.get(word, 0xFFFF).to_bytes(2, "big")
    payload = image[2 * (CRC_START_WORD - first):]
    if len(payload) % 2 or not payload:
        print("FAIL  empty payload")
        return 1
    payload_words = len(payload) // 2
    if payload_words > MAX_PAYLOAD_WORDS:
        print(f"FAIL  {payload_words} payload words exceed the 367-packet limit")
        return 1

    header = (MAGIC + payload_words.to_bytes(4, "big")
              + (zlib.crc32(payload) & 0xFFFFFFFF).to_bytes(4, "big"))
    packed = bytearray(image)
    packed[4:4 + len(header)] = header
    if len(packed) != len(image) or bytes(packed[0:4]) != bytes(image[0:4]):
        print("FAIL  header injection changed the image layout")
        return 1
    target.write_bytes(packed)

    print(f"input     : {source} ({len(words)} words, 0x{first:06X}..0x{last:06X})")
    print(f"payload   : {payload_words} words from word 0x{CRC_START_WORD:06X}")
    print(f"crc32     : {zlib.crc32(payload) & 0xFFFFFFFF:#010x}")
    print(f"written   : {target} ({len(packed)} bytes, "
          f"{(len(packed) + 1023) // 1024} packets)")
    print("next      : python tools/dsp_image_check.py " + str(target))
    return 0


if __name__ == "__main__":
    sys.exit(main())
