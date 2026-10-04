"""Idle-pacing probe (MSVC, x86): an lfence right before the IDLE pass's clock read, so the read is ordered after
the pass's loads the way the Linux vDSO's `lfence; rdtsc` orders it. Busy passes are untouched (the clock is read
on idle passes only). usage: patch-lfence.py <VirtualCore.cpp>"""
import io, sys
p = sys.argv[1]
s = io.open(p, encoding="utf-8", newline="").read()
old = "            const auto now = qb::mono_now();\n"
assert s.count(old) == 1
s = s.replace(old, "#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))\n            _mm_lfence();\n#endif\n" + old)
first_include = s.index("#include")
s = s[:first_include] + "#if defined(_MSC_VER)\n#include <intrin.h>\n#endif\n" + s[first_include:]
io.open(p, "w", encoding="utf-8", newline="").write(s)
print("PATCH-LFENCE-OK")
