#!/usr/bin/env python3
"""Reconstruct esp32_gateway_simulator.py from corrupted file."""
import ast
import sys

INPUT = r'd:/CS_INV_WIFI/tools/esp32_gateway_simulator.py.recovered'
OUTPUT = r'd:/CS_INV_WIFI/tools/esp32_gateway_simulator.py'

content = open(INPUT, 'r', encoding='utf-8').read()

SHEBANG = '#!/usr/bin/env python3'

# Find all shebang positions
positions = []
start = 0
while True:
    pos = content.find(SHEBANG, start)
    if pos == -1:
        break
    positions.append(pos)
    start = pos + len(SHEBANG)

print(f"Total shebang occurrences: {len(positions)}")

# Split into segments between shebangs
# Each shebang starts a new "copy" of the file
# The segment from shebang[i] to shebang[i+1] is one partial copy
segments = []
for i in range(len(positions)):
    seg_start = positions[i]
    seg_end = positions[i + 1] if i + 1 < len(positions) else len(content)
    seg = content[seg_start:seg_end]
    # The next shebang may have been concatenated on the last line
    # Remove trailing partial line that merges into next shebang
    segments.append(seg)

print(f"Segments: {len(segments)}")
lengths = sorted([s.count('\n') for s in segments], reverse=True)
print(f"Top 5 segment line counts: {lengths[:5]}")

# Each segment is a copy of (a prefix of) the original file.
# Strategy: for each line position, use majority voting across all segments.
# Split each segment into lines
seg_lines = []
for seg in segments:
    lines = seg.split('\n')
    # Remove trailing empty element from split
    if lines and lines[-1] == '':
        lines = lines[:-1]
    seg_lines.append(lines)

max_len = max(len(sl) for sl in seg_lines)
print(f"Max segment length: {max_len} lines")

# For each line position, collect candidates from all segments that reach that position
merged = []
for pos in range(max_len):
    candidates = {}
    for sl in seg_lines:
        if pos < len(sl):
            line = sl[pos]
            if line not in candidates:
                candidates[line] = 0
            candidates[line] += 1
    
    # Pick the most common line; break ties by preferring longer lines (more complete)
    best = max(candidates.keys(), key=lambda k: (candidates[k], len(k)))
    merged.append(best)

result = '\n'.join(merged) + '\n'
print(f"Merged: {len(merged)} lines, {len(result)} bytes")

# Try to parse
try:
    ast.parse(result)
    print("AST parse: OK!")
    with open(OUTPUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write(result)
    print(f"Written to {OUTPUT}")
except SyntaxError as e:
    print(f"AST error at line {e.lineno}: {e.msg}")
    # Show context
    for i in range(max(0, e.lineno - 5), min(len(merged), e.lineno + 5)):
        marker = ">>>" if i + 1 == e.lineno else "   "
        print(f"{marker} {i+1}: {merged[i][:120]}")
    
    # Save anyway for inspection
    with open(OUTPUT + '.partial', 'w', encoding='utf-8', newline='\n') as f:
        f.write(result)
    print(f"Saved partial to {OUTPUT}.partial")
    sys.exit(1)
