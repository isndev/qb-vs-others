"""Print the symbols that share a 64-byte line (and the adjacent lines) with a target symbol in an MSVC /MAP file.
usage: map-neighbors.py <file.map> <substring-of-decorated-name> [lines-around]"""
import re, sys

path, needle = sys.argv[1], sys.argv[2]
around = int(sys.argv[3]) if len(sys.argv) > 3 else 1
syms = []
rx = re.compile(r"^\s*([0-9a-fA-F]{4}):([0-9a-fA-F]{8})\s+(\S+)\s+([0-9a-fA-F]{16}|[0-9a-fA-F]{8})\s+(.*)$")
in_pub = False
for ln in open(path, encoding="utf-8", errors="replace"):
    if "Publics by Value" in ln:
        in_pub = True
        continue
    if in_pub and ln.startswith(" Static symbols"):
        pass
    m = rx.match(ln)
    if m and in_pub:
        syms.append((int(m.group(4), 16), m.group(1), m.group(3), m.group(5).strip()))
syms.sort()
hits = [s for s in syms if needle in s[2]]
if not hits:
    sys.exit("no symbol matches " + needle)
for rva, sec, name, obj in hits:
    line = rva // 64
    print(f"TARGET {name} rva=0x{rva:x} line=0x{line:x} off={rva % 64} sec={sec}")
    for s in syms:
        if abs(s[0] // 64 - line) <= around:
            mark = "  <== same line" if s[0] // 64 == line else ""
            print(f"   0x{s[0]:x} (+{s[0] - rva:5d}) sec={s[1]} {s[2][:110]}  [{s[3][-40:]}]{mark}")
