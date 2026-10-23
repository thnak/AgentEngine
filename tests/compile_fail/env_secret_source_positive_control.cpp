// Positive control for env_secret_source_no_default.cpp (ADR-244 slice 6): the same source, constructed through
// `create()` with a stated prefix, compiles.

#include "agentengine/trust/secret.hpp"

int main() {
    auto source = agentengine::EnvSecretSource::create(agentengine::kLegacyEnvSecretPrefix);
    return source.has_value() && (*source)->get("x").has_value() ? 1 : 0;
}
