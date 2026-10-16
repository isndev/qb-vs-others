// Reports the real qb adapter caveats for its inherited build profile; no actors are created.
#include "../../frameworks/qb/qb_support.h"

#include <cstdio>

int main() {
    for (const auto &caveat : qvoqb::caveats())
        std::puts(caveat.c_str());
    return 0;
}
