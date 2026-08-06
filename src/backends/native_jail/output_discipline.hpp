#pragma once
// Implements 010-Python-Code-Interpreter.md §3 items 4/5 ("Output discipline") -- Milestone 3 Phase
// F3. Applied to the MODEL-VISIBLE fields of `ExecOutcome` (`stdout_text`, `stderr_text`,
// `result_repr`) at the boundary where `MediatedPythonRunner`/`MediatedShellRunner::run()` return --
// a layer ABOVE `ResourceLimits::output_bytes` (008 §2), which is already real, already enforced,
// host-safety infrastructure: `native_jail_backend.cpp`/`linux_native_jail_backend.cpp`'s
// `drain_pipe_bounded()` already caps the RAW bytes a spawned child process can make this host
// buffer, before this file ever sees them. That bound exists so a hostile or runaway child cannot
// exhaust host memory; it says nothing about what belongs in a MODEL's prompt. This file's cap is
// the separate, always-applied concern 010 §3 names: even output that comfortably fits under the
// host-safety ceiling can still be far too large to hand an LLM, so every value that reaches the
// transcript is capped again here, with an explicit marker naming what was cut -- never a silent
// truncation a reader could mistake for the whole value.
//
// 006 §7 says this threshold should be "derived from the run's effective per-turn token budget
// (005 §3's TokenBudget)... scaled to a declared fraction, never a global byte constant." That
// plumbing does not exist anywhere in this codebase yet -- no TokenBudget value reaches
// EffectContext/ExecState/ResourceLimits at any call site (023 stays TBD-baselined project-wide
// until M8, matching the milestone-3 breakdown doc's own note for every other budget-derived gate:
// ADR-011's egress proxy, ADR-010's WASM host, this file all share the identical residual). Until a
// real budget is wired through, `kDefaultOutputCapBytes` below is a NAMED, provisional stand-in -- a
// fixed byte constant is exactly what 006 §7 says is not a real answer, so this is a residual to
// replace once 023 lands, not a design choice presented as settled. `MediatedPythonConfig`/
// `MediatedShellRunner`'s own `output_cap_bytes` field (both host-configured, per-session inputs, the
// same pattern `mount_roots`/`tool_bridge` already use) lets a caller override the default per
// session in the meantime, exactly as it would need to once a real per-turn value is available.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace agentengine::native_jail {

inline constexpr std::uint64_t kDefaultOutputCapBytes = 64ull * 1024;  // see file header: provisional

struct CappedOutput {
    std::string text;
    bool truncated = false;
    std::uint64_t original_bytes = 0;
};

// Truncates `text` to at most `cap_bytes`, backing off to the nearest UTF-8 character boundary
// (never splitting a multi-byte codepoint mid-sequence), and appends an explicit marker naming
// exactly how much was kept and how much the original was -- 010 §3's "explicit truncation markers"
// requirement, made concrete. A no-op (returns `text` unchanged, `truncated=false`) when `text`
// already fits.
[[nodiscard]] inline CappedOutput cap_output(std::string text, std::uint64_t cap_bytes) {
    std::uint64_t const original = text.size();
    if (original <= cap_bytes) {
        return CappedOutput{std::move(text), false, original};
    }
    std::size_t cut = static_cast<std::size_t>(cap_bytes);
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    std::string kept = text.substr(0, cut);
    kept += "\n...[truncated: showing " + std::to_string(cut) + " of " + std::to_string(original) +
            " bytes]";
    return CappedOutput{std::move(kept), true, original};
}

}  // namespace agentengine::native_jail
