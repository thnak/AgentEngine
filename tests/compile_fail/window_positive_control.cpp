// Positive control for window_zero_rejected: the same program with a real window and with the named unbounded one.

#include "agentengine/core/history_provider.hpp"

int main() {
    agentengine::HistoryProvider<agentengine::Window<4>>   bounded;
    agentengine::HistoryProvider<agentengine::UnboundedWindow> unbounded;
    (void)bounded;
    (void)unbounded;
    return 0;
}
