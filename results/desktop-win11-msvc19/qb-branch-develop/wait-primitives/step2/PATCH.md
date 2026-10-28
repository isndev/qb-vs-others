# QB-181 step 2 -- the `tpause` pacing variant, the exact change (scratch copy of qb `e152216a`, never the repository)

Applied by `qb181-patch-pace.py` to `src/qb/core/VirtualCore.cpp` of the export; the control is the untouched export.

```diff
--- qb/src/qb/core/VirtualCore.cpp	2026-10-27 15:59:12.000000000 +0100
+++ qb/src/qb/core/VirtualCore.cpp	2026-10-28 08:27:03.725325400 +0100
@@ -959,8 +959,59 @@
 // synchronisation -- was level on those two and lost the same-core round trip 2 %. A tight poll
 // wins a ping-pong and loses a streaming producer there too (qb-vs-others TUNING.md section
 // 16.2 on x86, 23.1 on arm64), so the ordered read stays on every architecture.
+// QB-181 EXPERIMENT (this scratch tree only): the idle poll paced by WAITPKG's `tpause` -- a pause of a chosen length,
+// 40 TSC ticks (~13 ns on an i9-12900K, the clock read's own length), to a deadline -- in place of the ordered counter
+// read, when CPUID.(7,0):ECX[5] says the host has WAITPKG; the ordered read otherwise. Judged against the tip by the
+// QB-1020 census on savina/ping-pong and thread-ring at two cores and by dev/bench's Multi_PingPong (TUNING.md 28.4).
+#if defined(__x86_64__) || defined(_M_X64)
+#if defined(_MSC_VER)
+#include <immintrin.h>
+#include <intrin.h>
+#else
+#include <cpuid.h>
+#include <immintrin.h>
+#endif
+namespace {
+constexpr std::uint64_t kQb181TpauseTicks = 40;
+
+bool
+qb181_cpu_has_waitpkg() noexcept {
+#if defined(_MSC_VER)
+    int r[4];
+    __cpuidex(r, 7, 0);
+    return ((r[2] >> 5) & 1) != 0;
+#else
+    unsigned a = 0, b = 0, c = 0, d = 0;
+    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d))
+        return false;
+    return ((c >> 5) & 1) != 0;
+#endif
+}
+
+#if defined(_MSC_VER)
+inline void
+qb181_tpause_until(std::uint64_t const deadline) noexcept {
+    _tpause(1u /* C0.1 */, deadline);
+}
+#else
+__attribute__((target("waitpkg"))) inline void
+qb181_tpause_until(std::uint64_t const deadline) noexcept {
+    _tpause(1u /* C0.1 */, deadline);
+}
+#endif
+} // namespace
+#endif
+
 std::uint64_t
 VirtualCore::__pace__() noexcept {
+#if defined(__x86_64__) || defined(_M_X64)
+    static const bool waitpkg = qb181_cpu_has_waitpkg();
+    if (waitpkg) {
+        qb181_tpause_until(qb::tsc_ticks() + kQb181TpauseTicks);
+        _poll_mark = qb::tsc_ticks();
+        return _poll_mark;
+    }
+#endif
     std::uint64_t t = qb::detail::tsc_ticks_ordered();
     while (t - _poll_mark < _poll_floor_ticks)
         t = qb::detail::tsc_ticks_ordered();
```
