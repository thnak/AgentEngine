#pragma once
// PROVE-PHASE shared shim: a minimal `result<T>` matching the SHAPE agentengine::result<T> uses
// (std::expected-based, an `error` struct with a message + machine code) without pulling in the real
// core/error.hpp -- these probes are standalone by design (§0's no-reuse-at-design-time framing), and
// this file exists only so probe code reads the same way the real design doc's pseudocode does.

#include <expected>
#include <string>

namespace probe {

struct error {
    std::string message;
    std::string code;
};

template <class T>
using result = std::expected<T, error>;

}  // namespace probe
