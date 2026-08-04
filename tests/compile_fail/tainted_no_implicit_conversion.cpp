// This file MUST NOT compile (007-Capability-and-Trust-Model.md §9 G2) — see tests/CMakeLists.txt's
// try_compile gate, which fails the build if it does. `Tainted<T>` has no implicit conversion to
// `T`/`T const&`, so a tainted value cannot silently flow into an API shaped like the
// capability-granting/policy-deciding surface 007 §4 forbids it from reaching.
//
// Tested directly against `T const&` (`std::string const&`), not `std::string_view`: C++ overload
// resolution allows at most ONE user-defined conversion per implicit conversion sequence, so
// `Tainted<T> -> string_view` would need two hops (`Tainted<T> -> T` via a hypothetical `Tainted`
// conversion, then `T -> string_view` via `std::string`'s own conversion operator) and could never
// compile regardless of what `Tainted<T>` itself provides — that would make the proof accidentally
// pass because of an unrelated standard-library rule, not because `Tainted<T>` is actually sound.
// Testing against `T const&` exercises the one hop `Tainted<T>` itself controls, which is the
// property 007 §4 actually needs (T is generic here, not always something with its own string_view
// conversion).

#include <string>

#include "agentengine/core/tainted.hpp"

namespace {
void capability_shaped_api(std::string const&) {}
} // namespace

int main() {
    agentengine::Tainted<std::string> t{std::string{"model output"}};
    capability_shaped_api(t);  // must fail: no implicit Tainted<T> -> T const& conversion
    return 0;
}
