"""Pacing probe (diagnostic only, on the CANDIDATE): put back the parent's per-pass stop-token poll in front of the
QB-65 check -- same signal logic, the same incidental per-pass work as before QB-65. If the 2c spin cells come back
level, their loss is the idle-poll pacing QB-180 documents, not the signal logic. usage: patch-s2.py <VirtualCore.cpp>"""
import io, sys
p = sys.argv[1]
s = io.open(p, encoding="utf-8", newline="").read()
old = """        if (unlikely(Main::_signal_generation.load(std::memory_order_relaxed) != scanned_generation))
            scanned_generation = __deliver_signals__();
"""
new = """        const bool stop_poll = _stop_token.stop_possible() && _stop_token.stop_requested();
        if (unlikely(Main::_signal_generation.load(std::memory_order_relaxed) != scanned_generation || stop_poll))
            scanned_generation = __deliver_signals__();
"""
assert s.count(old) == 1
s = s.replace(old, new)
io.open(p, "w", encoding="utf-8", newline="").write(s)
print("PATCH-S2-OK")
