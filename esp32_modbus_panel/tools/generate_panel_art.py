"""Pack the dashboard house and native-size device artwork for LVGL."""
import argparse
from collections import Counter
import hashlib
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
BLOCK_ROWS = 16


def pack_rows(rows):
    """Encode palette indices with literals, solid spans and copies from the preceding row."""
    width = len(rows[0])
    offsets, encoded = [], bytearray()
    for y, row in enumerate(rows):
        above = rows[y - 1] if y % BLOCK_ROWS else None
        if above is None:
            offsets.append(len(encoded))
        cost = [0] * (width + 1)
        choices = [None] * width
        for x in range(width - 1, -1, -1):
            pixel = row[x]
            best = (cost[x + 1] + (1 if pixel < 64 else 2), 0, 1)
            same = copy = True
            for length in range(1, min(64, width - x) + 1):
                end = x + length
                same &= row[end - 1] == pixel
                copy = copy and above is not None and row[end - 1] == above[end - 1]
                best = min(best, (cost[end] + 1 + length, 0, length))
                if same:
                    best = min(best, (cost[end] + 2, 1, length))
                if copy:
                    best = min(best, (cost[end] + 1, 2, length))
            cost[x], kind, length = best
            choices[x] = kind, length

        x = 0
        while x < width:
            kind, length = choices[x]
            if kind == 2:
                encoded.append(0xc0 | (length - 1))
            elif kind == 1:
                encoded.extend((0x80 | (length - 1), row[x]))
            elif length == 1 and row[x] < 64:
                encoded.append(row[x])
            else:
                encoded.append(0x40 | (length - 1))
                encoded.extend(row[x:x + length])
            x += length
    check_rows(rows, offsets, encoded)
    return offsets, encoded


def check_rows(rows, offsets, data):
    """Compare the decoded result to every source pixel, including block boundaries."""
    width = len(rows[0])
    line = bytearray(width)
    cursor = 0
    for y, expected in enumerate(rows):
        if y % BLOCK_ROWS == 0:
            assert cursor == offsets[y // BLOCK_ROWS]
            line = bytearray(width)
        x = 0
        while x < width:
            token = data[cursor]
            cursor += 1
            length = (token & 63) + 1 if token >= 64 else 1
            if token < 64:
                line[x] = token
            elif token < 128:
                line[x:x + length] = data[cursor:cursor + length]
                cursor += length
            elif token < 192:
                line[x:x + length] = bytes([data[cursor]]) * length
                cursor += 1
            else:
                assert y % BLOCK_ROWS
            x += length
        assert x == width and line == bytes(expected)
    assert cursor == len(data)


def array(kind, name, values, fmt, columns):
    lines = [f"static const {kind} {name}[]={{"]
    for start in range(0, len(values), columns):
        lines.append("    " + ",".join(fmt(v) for v in values[start:start + columns]) + ",")
    lines.append("};")
    return "\n".join(lines)


def asset(name, palette, rows):
    offsets, encoded = pack_rows(rows)
    colors = bytearray()
    for rgba in palette:
        r, g, b, a = rgba.to_bytes(4, "big")
        rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        colors.extend((rgb565 & 255, rgb565 >> 8, a))
    return "\n".join([
        array("uint8_t", f"{name}_palette", colors, lambda v: f"0x{v:02x}", 24),
        array("uint16_t", f"{name}_blocks", offsets, str, 16),
        array("uint8_t", f"{name}_pixels", encoded, lambda v: f"0x{v:02x}", 20),
        f"static const panel_indexed_art_t {name}_art="
        f"{{{len(rows[0])},{len(rows)},{name}_palette,{name}_blocks,{name}_pixels}};",
    ])


def house():
    from PIL import Image

    source = ROOT.parent / "esp32_direct/main/direct/direct_home_asset.c"
    text = source.read_text(encoding="utf-8")

    def extract(name):
        return [int(v) for v in re.findall(r"\d+", re.search(name + r"\[\d+\]=\{(.*?)\};", text, re.S)[1])]

    palette_bytes = bytes(extract("palette"))
    palette = [palette_bytes[i:i + 4] for i in range(0, len(palette_bytes), 4)]
    offsets, packed = extract("row_offsets"), extract("packed_indices")
    rows = []
    for start, end in zip(offsets, offsets[1:]):
        row = []
        while start < end:
            token = packed[start]
            start += 1
            count = (token & 127) + 1
            if token & 128:
                row.extend([packed[start]] * count)
                start += 1
            else:
                row.extend(packed[start:start + count])
                start += count
        assert len(row) == 320
        rows.append(row)
    assert len(rows) == 192
    rgba = b"".join(palette[pixel] for row in rows for pixel in row)
    image = Image.frombytes("RGBA", (320, 192), rgba).crop((96, 16, 310, 174))
    # The dashboard draws its own connection lines. Clear the small cable span
    # left outside the wall after cropping the utility pole out of the source.
    image.paste((0, 0, 0, 0), (0, 82, 18, 99))
    image = image.resize((116, 86), Image.Resampling.LANCZOS)
    image.putalpha(image.getchannel("A").point(lambda a: 0 if a <= 3 else 255 if a >= 252 else a))
    indexed = image.quantize(colors=128, method=Image.Quantize.FASTOCTREE)

    packed_palette = bytes(indexed.getpalette("RGBA"))
    pixels = indexed.tobytes()
    frequency = Counter(pixels)
    order = sorted(frequency, key=lambda index: (-frequency[index], index))
    indices = {source: target for target, source in enumerate(order)}
    remap = bytes(indices.get(index, 0) for index in range(256))
    palette = [int.from_bytes(packed_palette[index * 4:index * 4 + 4], "big") for index in order]
    rows = [pixels[i:i + image.width].translate(remap) for i in range(0, len(pixels), image.width)]
    return "/* Cropped and resized from the original Direct house illustration. */\n" + asset(
        "house", palette, rows)


def devices(path):
    from PIL import Image

    atlas = Image.open(path).convert("RGBA")
    assert atlas.size == (1536, 1024)
    variants = [
        ("storage", 0, 100), ("inverter_editor", 1, 20), ("inverter", 1, 106),
        ("solar", 2, 44), ("power", 3, 44), ("alarm", 4, 44), ("gear", 5, 44),
    ]
    out = [f"/* Native-size device artwork. Source SHA256: {hashlib.sha256(path.read_bytes()).hexdigest()} */"]
    for name, index, height in variants:
        col, row = index % 3, index // 3
        item = atlas.crop((col * 512, 555 if row else 0, (col + 1) * 512, 1024 if row else 539))
        x0, y0, x1, y1 = item.getchannel("A").point(lambda n: 255 if n >= 125 else 0).getbbox()
        item = item.crop((x0 - 2, y0 - 2, x1 + 2, y1 + 2))
        width = round(item.width * height / item.height)
        item = item.resize((width, height), Image.Resampling.LANCZOS)
        item.putalpha(item.getchannel("A").point(lambda n: 0 if n < 28 else 255 if n >= 246 else n))
        # Reserve palette entries for the tiny indicator lights in the silver devices.
        accent = [(0x29, 0xa7, 0xff), (0xff, 0xd4, 0x47), (0x4d, 0xd8, 0x83)] if index <= 1 else []
        # The 48-entry palette retains more shading and edge alpha at panel size.
        count = 48 - len(accent)
        quantized = item.quantize(colors=count, method=Image.Quantize.FASTOCTREE)
        rgba = bytes(quantized.getpalette("RGBA")[:4 * count])
        palette = [int.from_bytes(rgba[i:i + 4], "big") for i in range(0, len(rgba), 4)]
        palette.extend(int.from_bytes(bytes((*color, 255)), "big") for color in accent)
        indices = bytearray(quantized.tobytes())
        if accent:
            for pos, (r, g, b, a) in enumerate(item.getdata()):
                if a < 200:
                    continue
                if b >= r + 70 and g >= r + 40:
                    indices[pos] = count
                elif index == 1 and r >= b + 70 and g >= b + 45:
                    indices[pos] = count + 1
                elif index == 1 and g >= r + 55 and g >= b + 55:
                    indices[pos] = count + 2
        rows = [indices[i:i + width] for i in range(0, len(indices), width)]
        out.append(asset(name, palette, rows))
    return "\n".join(out)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device-atlas", type=Path)
    args = parser.parse_args()
    (ROOT / "full/panel_home_art.inc").write_text(house() + "\n", encoding="ascii")
    if args.device_atlas:
        (ROOT / "full/panel_device_art.inc").write_text(devices(args.device_atlas) + "\n", encoding="ascii")
    print("PASS indexed artwork: every source row round-trips through the packed data")
