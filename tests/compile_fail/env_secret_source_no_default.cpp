// This file MUST NOT compile (decisions/ADR-244-required-choices-step-1b.md §2.2, slice 6): an `EnvSecretSource` with
// no stated prefix. The constructor is private and there is no default prefix; `EnvSecretSource::create(prefix)` is
// the only way in and refuses an empty prefix at run time (`secret.empty_prefix`).

#include "agentengine/trust/secret.hpp"

int main() {
    agentengine::EnvSecretSource source;  // no prefix stated
    return source.get("x").has_value() ? 1 : 0;
}
