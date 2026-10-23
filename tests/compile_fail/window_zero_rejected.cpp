// This file MUST NOT compile (decisions/ADR-244-required-choices-step-1b.md §2.2, slice 5): `Window<0>` was "keep
// everything" by way of a `0`. It is not a window now; `UnboundedWindow` is the named spelling.

#include "agentengine/core/history_provider.hpp"

int main() {
    agentengine::HistoryProvider<agentengine::Window<0>> provider;
    (void)provider;
    return 0;
}
