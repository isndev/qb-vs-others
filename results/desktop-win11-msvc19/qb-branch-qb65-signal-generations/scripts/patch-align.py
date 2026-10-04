"""Layout-pinning variant: VirtualCore aligned to a cache line, so its members' line grouping no longer depends on
where the worker's stack frame happens to put it. usage: patch-align.py <VirtualCore.h>"""
import io, sys
p = sys.argv[1]
s = io.open(p, encoding="utf-8", newline="").read()
old = "\nclass VirtualCore {\n"
assert s.count(old) == 1
s = s.replace(old, "\nclass alignas(QB_LOCKFREE_CACHELINE_BYTES) VirtualCore {\n")
io.open(p, "w", encoding="utf-8", newline="").write(s)
print("PATCH-ALIGN-OK")
