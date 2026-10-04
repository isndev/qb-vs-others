"""Pass-speed probe (diagnostic only, on the PARENT): drop the per-pass stop-token poll and nothing else, so the pass
gets faster for a reason unrelated to QB-65's signal logic. The benches end by kill(), never by the token.
usage: patch-nostop.py <VirtualCore.cpp>"""
import io, sys
p = sys.argv[1]
s = io.open(p, encoding="utf-8", newline="").read()
old = "        const bool stop_requested = _stop_token.stop_possible() && _stop_token.stop_requested();\n"
assert s.count(old) == 1
s = s.replace(old, "        const bool stop_requested = false; // diagnostic: no per-pass token poll\n")
io.open(p, "w", encoding="utf-8", newline="").write(s)
print("PATCH-NOSTOP-OK")
