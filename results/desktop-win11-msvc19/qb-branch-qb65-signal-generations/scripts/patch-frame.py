"""Frame-alignment probe (diagnostic only): give VirtualCore::__workflow__ a 64-byte-aligned stack object, as the
parent's inline SignalEvent was, so MSVC realigns the frame (frame pointer in rbp) exactly as it did before QB-65.
One volatile store at loop entry; nothing per pass. usage: patch-frame.py <VirtualCore.cpp>"""
import io, sys
p = sys.argv[1]
s = io.open(p, encoding="utf-8", newline="").read()
anchor = "    auto &loop = io::async::listener::current;\n"
assert s.count(anchor) == 1
s = s.replace(anchor, anchor + "    alignas(64) volatile unsigned char qb65_frame_probe[64];\n    qb65_frame_probe[0] = 0;\n")
io.open(p, "w", encoding="utf-8", newline="").write(s)
print("PATCH-FRAME-OK")
