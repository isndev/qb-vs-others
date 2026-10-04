"""Pure-layout variant for the QB-65 bisect: append to VirtualCore, after _stop_token, a member read by nothing.
usage: patch-lay.py <VirtualCore.h> [bytes]   (default: exactly _signal_seen's size, NSIG unsigned ints)"""
import io, sys
p = sys.argv[1]
nbytes = int(sys.argv[2]) if len(sys.argv) > 2 else 0
s = io.open(p, encoding="utf-8", newline="").read()
old = "    qb::stop_token _stop_token;\n"
assert s.count(old) == 1
if nbytes:
    member = f"    std::array<unsigned char, {nbytes}> _signal_seen_lay{{}};\n"
else:
    member = "    std::array<unsigned int, Main::SignalSlotsLay> _signal_seen_lay{};\n"
s = s.replace(old, old + member)
io.open(p, "w", encoding="utf-8", newline="").write(s)
if not nbytes:
    m = p.replace("VirtualCore.h", "Main.h")
    t = io.open(m, encoding="utf-8", newline="").read()
    anchor = "    static std::atomic<unsigned int> _signal_generation;\n"
    assert t.count(anchor) == 1
    t = t.replace(anchor, anchor + "#if defined(NSIG)\n    static constexpr std::size_t SignalSlotsLay = NSIG;\n#else\n"
                  "    static constexpr std::size_t SignalSlotsLay = 65;\n#endif\n")
    io.open(m, "w", encoding="utf-8", newline="").write(t)
print("PATCH-LAY-OK", nbytes or "NSIG*4")
