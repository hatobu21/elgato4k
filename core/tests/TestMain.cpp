#include "Check.h"

int main() {
    int failedCases = 0;
    for (const auto& c : vela::test::registry()) {
        const int before = vela::test::failures();
        c.body();
        const bool ok = vela::test::failures() == before;
        if (!ok) ++failedCases;
        std::printf("[%s] %s\n", ok ? " ok " : "FAIL", c.name);
    }
    std::printf("\n%zu cases, %d failed\n", vela::test::registry().size(), failedCases);
    return failedCases == 0 ? 0 : 1;
}
