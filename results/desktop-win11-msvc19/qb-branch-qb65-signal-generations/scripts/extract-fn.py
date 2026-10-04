"""Extract one function's disassembly from `dumpbin /disasm:nobytes` output, using the /MAP for its address range
(start = its RVA+base, end = the next symbol's address in the same section).
usage: extract-fn.py <disasm.txt> <file.map> <decorated-name-substring> <out.txt>"""
import re, sys
dis, mp, needle, out = sys.argv[1:5]
rx = re.compile(r"^\s*([0-9a-fA-F]{4}):([0-9a-fA-F]{8})\s+(\S+)\s+([0-9a-fA-F]{16})\s+")
syms = []
pub = False
for ln in open(mp, encoding="utf-8", errors="replace"):
    if "Publics by Value" in ln:
        pub = True
        continue
    m = rx.match(ln)
    if m and pub and m.group(1) == "0001":
        syms.append((int(m.group(4), 16), m.group(3)))
syms.sort()
idx = [i for i, s in enumerate(syms) if s[1] == needle]
assert len(idx) == 1, [syms[i][1] for i in idx]
start, name = syms[idx[0]]
end = syms[idx[0] + 1][0]
lines = []
ad = re.compile(r"^\s+([0-9A-F]{16}):\s+(.*)$")
for ln in open(dis, encoding="utf-8", errors="replace"):
    m = ad.match(ln)
    if m:
        a = int(m.group(1), 16)
        if start <= a < end:
            lines.append(f"{a - start:5x}: {m.group(2)}")
open(out, "w", encoding="utf-8").write("\n".join(lines) + "\n")
print(f"{name}: 0x{start:x}..0x{end:x} ({end - start} bytes, {len(lines)} instructions) -> {out}")
