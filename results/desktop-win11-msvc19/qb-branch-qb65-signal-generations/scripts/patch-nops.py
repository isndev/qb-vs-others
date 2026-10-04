"""Code-alignment probe (diagnostic only, MSVC): insert N one-byte NOPs at the top of VirtualCore's pass, right after
the pass counter, so every later instruction of the loop body moves by N bytes and nothing else changes.
usage: patch-nops.py <VirtualCore.cpp> <N>"""
import io, sys
p, n = sys.argv[1], int(sys.argv[2])
s = io.open(p, encoding="utf-8", newline="").read()
anchor = "        ++_loop_count; // 1-based loop-pass index surfaced to callbacks via qb::LoopEvent; also keys the `time()` sample\n"
assert s.count(anchor) == 1
s = s.replace(anchor, anchor + "#if defined(_MSC_VER)\n" + "        __nop();\n" * n + "#endif\n")
first_include = s.index("#include")
s = s[:first_include] + "#if defined(_MSC_VER)\n#include <intrin.h>\n#endif\n" + s[first_include:]
io.open(p, "w", encoding="utf-8", newline="").write(s)
print("PATCH-NOPS-OK", n)
