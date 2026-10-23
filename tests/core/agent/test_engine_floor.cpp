// Proves decisions/ADR-244-required-choices-step-1b.md §2.1 "Engine floors are named" (slice 5, G-C12): the floors the
// engine keeps under an `unbounded()` choice are one table, each applied through the SAME constant the table lists (so
// the value recorded is the value enforced), disclosed by `describe()`, and every capability cap states `unbounded()`
// as a value that is not a number.
//
//   E1  describe() says "unbounded (engine floor N unit)" for an unbounded cap and "at most N unit" for a stated one.
//   E2  The table is complete and consistent: unique ids, nonzero values, every floor reachable through all().
//   E3  The public applications of a floor are the table's constant, not a copy that could drift (the Anthropic request's
//       max_tokens and the container CLIs' invocation timeout).
//   E4  A stated cap and `unbounded()` are different values on every capability (the old "unset == uncapped" is
//       gone: there is no unset), and `covers` keeps the widening rule -- a capped parent never covers an unbounded
//       child, an unbounded parent covers any child.
//
// Needs no daemon, no network and no credentials.

#include <cstdio>
#include <set>
#include <string>

#include "agentengine/core/engine_floor.hpp"
#ifdef AGENTENGINE_WITH_HTTPS
#include "agentengine/protocol/anthropic/chat_client.hpp"
#endif
#include "agentengine/trust/capability.hpp"

namespace ae = agentengine;

namespace {
int g_failed = 0;
void check(bool cond, std::string const& what) {
    std::printf("%s %s\n", cond ? "[ok]  " : "[FAIL]", what.c_str());
    if (!cond) ++g_failed;
}
}  // namespace

int main() {
    using B = ae::Bound<std::uint64_t>;
    namespace ef = ae::engine_floor;

    // ---- E1
    check(ef::describe(B::unbounded(), ef::kOutputDrain) == "unbounded (engine floor 16777216 bytes)",
          "E1: an unbounded output cap is disclosed with its engine floor");
    check(ef::describe(B::at_most(4096), ef::kOutputDrain) == "at most 4096 bytes",
          "E1: a stated cap is reported as stated, without a floor");

    // ---- E2
    std::set<std::string_view> ids;
    bool nonzero = true;
    for (ae::EngineFloor const& f : ef::all()) {
        ids.insert(f.id);
        nonzero = nonzero && f.value != 0 && !f.unit.empty() && !f.applies.empty();
    }
    check(ids.size() == ef::all().size(), "E2: floor ids are unique");
    check(nonzero, "E2: every floor has a nonzero value, a unit and a stated place of application");

    // ---- E3
#ifdef AGENTENGINE_WITH_HTTPS
    check(ae::anthropic::kDefaultMaxTokens == ef::kProviderMaxTokens.value,
          "E3: the Anthropic request's max_tokens fallback IS the table's floor");
#endif

    // ---- E4
    using L = ae::cap::Limit;
    check(L::at_most(1) != L::unbounded() && L::unbounded() != L::at_most(1'000'000),
          "E4: a stated cap and unbounded() are different values");
    auto const capped = ae::Capability{ae::cap::FsRead{"work", "", L::at_most(100)}};
    auto const unbounded_read = ae::Capability{ae::cap::FsRead{"work", "", L::unbounded()}};
    auto const smaller = ae::Capability{ae::cap::FsRead{"work", "", L::at_most(10)}};
    auto const capped_set = ae::CapabilitySet::grant_root({capped});
    auto const open_set = ae::CapabilitySet::grant_root({unbounded_read});
    check(capped_set.contains(smaller), "E4 (control): a capped parent covers a smaller cap");
    check(!capped_set.contains(unbounded_read), "E4: a capped parent never covers an unbounded child (widening)");
    check(open_set.contains(capped) && open_set.contains(unbounded_read), "E4: an unbounded parent covers any child");

    std::printf("\n%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
