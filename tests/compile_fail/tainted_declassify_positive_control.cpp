// Positive control for the 007-Capability-and-Trust-Model.md §9 G2 compile-fail proof — this file
// MUST compile. A fail-only suite can't distinguish "the mechanism correctly rejects this" from
// "nothing here compiles at all"; this proves the explicit, named declassifier path
// (`unsafe_view()`, 007 §4) still works.

#include <string>
#include <string_view>

#include "agentengine/core/tainted.hpp"

namespace {
void capability_shaped_api(std::string_view) {}
} // namespace

int main() {
    agentengine::Tainted<std::string> t{std::string{"model output"}};
    capability_shaped_api(t.unsafe_view());  // explicit declassifier — must compile
    return 0;
}
