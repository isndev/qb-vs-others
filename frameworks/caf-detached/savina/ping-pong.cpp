// caf-detached / savina/ping-pong -- the SAME adapter as frameworks/caf/savina/ping-pong.cpp,
// built with every actor `caf::detached`. See frameworks/caf/caf_support.h (kSpawnOptions) for
// why this variant exists and frameworks/caf-detached/README.md for how to read its row.
//
// One source, two binaries, on purpose: a second copy of the adapter would be a second place for
// the two to drift apart, and the whole point of the variant is that ONLY the placement differs.
#define QVO_CAF_DETACHED 1
#include "../../caf/savina/ping-pong.cpp"
