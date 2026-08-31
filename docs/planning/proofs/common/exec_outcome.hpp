#pragma once
// Shared across every prove-phase probe that shells out to run a real command (docker_sandbox/,
// execution_surface/) -- factored out here so both stay the SAME type rather than two
// independently-defined, identically-shaped structs that happen to collide when a later probe
// (execution_surface/) needs to compose with an earlier one (docker_sandbox/) in the same
// translation unit.

#include <string>

namespace probe {

struct ExecOutcome {
    int exit_code = -1;
    std::string stdout_text;
};

}  // namespace probe
