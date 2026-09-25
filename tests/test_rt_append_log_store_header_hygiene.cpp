// Proof for ADR-195 E32 red team round 3 (MAJOR): including the append-log store -- directly, or through
// core/memory.hpp and the worktree headers that pull it in -- must not bring <windows.h> or its macros
// into the includer. The header-only version did, and consumer code broke: `enum class log_level { INFO,
// ERROR }` hit windows.h's `ERROR` macro, and a leaked WIN32_LEAN_AND_MEAN stripped a consumer's own
// <shellapi.h>. The platform code now lives in src/rt/append_log_file.cpp.
//
// This file is the consumer. It includes nothing before the engine headers, so every check below is about
// what THEY define. If any leaks, this file does not compile -- the check is the build itself.

// This project defines both project-wide (CMakeLists.txt); a consumer's build does not. Drop them so this
// file sees what a consumer sees.
#undef NOMINMAX
#undef WIN32_LEAN_AND_MEAN

#include "agentengine/core/memory.hpp"
#include "agentengine/core/worktree.hpp"
#include "agentengine/rt/append_log_store.hpp"

#if defined(_WINDOWS_) || defined(WIN32_LEAN_AND_MEAN) || defined(NOMINMAX) || defined(ERROR) || \
    defined(GetMessage) || defined(CreateFile) || defined(min) || defined(max)
#error "an engine header leaked <windows.h> or its macros into the includer"
#endif

#include <cstdio>

// The consumer shapes the red team broke with the leak.
enum class log_level { INFO, ERROR };
struct Mailbox {
    int GetMessage() const { return 1; }
    int CreateFile() const { return 2; }
};

int main() {
    Mailbox const m;
    bool const ok = static_cast<int>(log_level::ERROR) == 1 && m.GetMessage() == 1 && m.CreateFile() == 2;
    std::fprintf(stderr, "%s: the store's headers leak no <windows.h> macros into an includer\n", ok ? "  ok" : "FAIL");
    return ok ? 0 : 1;
}
