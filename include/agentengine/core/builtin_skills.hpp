#pragma once
// Implements 009-Plugin-and-Extension-System.md §8f -- the "generic skills" that ship IN this engine,
// compiled into the binary, never as loose `SKILL.md` files a deployment's host filesystem must
// happen to still contain at runtime. "These ship in this repo, not as a separate download" (§8f's
// own words) is satisfied more directly by `InlineSkillSource` (skill_source.hpp) than by a
// `DiskSkillSource` pointed at a repo-relative path that a real deployment could omit or move --
// `DiskSkillSource` stays fully real and tested for its own actual use case (an operator- or
// third-party-authored skill directory on a real host filesystem), it is simply not how these five
// ship.
//
// All five of §8f's named skills now have real content: `using-the-code-interpreter`,
// `using-codeact`, `reading-large-content`, `producing-structured-output`, `shell-pipelines`. Each
// constant is written against the actual spec section it teaches (026 §5's `agent.*` module table,
// 006 §7's tool-result-hygiene rules, 003 §4's structured-output strategies, 010 §2's `ShellRunner`
// grammar) rather than generic advice, so a stale or wrong claim here is a real bug against those
// sections, not just prose.

#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/skill.hpp"
#include "agentengine/core/skill_source.hpp"

namespace agentengine {

inline constexpr std::string_view kUsingTheCodeInterpreterSkillMd = R"SKILL(---
name: using-the-code-interpreter
description: Idioms for the execute_code tool -- when one call suffices, when CodeAct's multi-step form pays for itself, and how session state persists across calls. Use this before writing Python to accomplish a task in this environment.
allowed-tools: execute_code
metadata:
  version: "1"
---
# Using the code interpreter

`execute_code(code, language="python") -> outputs, artifacts` runs inside this session's own
interpreter. In-memory state (variables, imports, open file handles) persists from one call to the
next within the same session -- it is not a fresh process every time.

## One call is usually enough

For a single, self-contained piece of work -- parse a file, compute a statistic, transform some
data, produce one artifact -- write it as ONE `execute_code` call, start to finish. Splitting an
obviously single-shot task into several round trips only adds latency and gives the model more
chances to lose track of intermediate state that was never going to leave the interpreter anyway.

## When the multi-step (CodeAct) form pays for itself

CodeAct is the SAME `execute_code` tool, used with the `agent` library present in the sandbox. Reach
for it, across several calls, when:

- A later step genuinely depends on a RESULT from an earlier step that is cheaper to inspect than to
  predict (e.g. the schema of data you haven't loaded yet).
- You would otherwise need to round-trip a large intermediate result through the model just to decide
  what to do with it next -- filter or summarize it in-process instead, and only bring the model the
  part that matters.
- The task is naturally a sequence of decisions, each informed by the last one's real output, not a
  single deterministic transformation you could already write end to end.

See the `using-codeact` skill for the `agent.*` module surface itself.

## What NOT to do

Do not re-derive state you already have. If a previous `execute_code` call already loaded or computed
something, reuse the SAME interpreter's live variables in the next call rather than re-reading a file
or recomputing a value from scratch -- persistence across calls is the whole point.
)SKILL";

inline constexpr std::string_view kUsingCodeactSkillMd = R"SKILL(---
name: using-codeact
description: Worked agent.* module examples for CodeAct -- filtering, transforming, and summarizing large intermediate results in-process instead of round-tripping every row through the model. Use this once a task needs several dependent execute_code steps, alongside the using-the-code-interpreter skill.
allowed-tools: execute_code
metadata:
  version: "1"
---
# Using CodeAct

CodeAct is not a different tool -- it is `execute_code` with the `agent` library present in the
sandbox. The library IS the action space: instead of the model naming one tool per action, it writes
ordinary Python against `agent.*` and the interpreter executes it under the same capability-gated
tool pipeline every other call goes through. Only the module the run was granted a capability for is
importable; a call to an ungranted module fails closed the same way an ungranted tool call would.

## The module surface

| Module | Purpose |
| --- | --- |
| `agent.tools` | Every granted tool, as a plain callable -- `agent.tools.search(query="...")` instead of a named tool-call round trip. |
| `agent.files` | Convenience over the mounted worktree -- `artifact()`, `input()`, listing directories. |
| `agent.data` | Tabular/JSON helpers over inputs without loading them wholly into memory. |
| `agent.memory` | Read access to the memory worktree's files under `/memory`. |
| `agent.notes` | Durable notes across turns and sessions -- writes land as agent-authored memory items, not plain file writes. |
| `agent.output` | Emit the run's final structured output, when one is declared -- see the producing-structured-output skill. |
| `agent.progress` | Report progress on long-running work without spending any of the turn's token budget -- only the final result reaches the transcript. |
| `agent.ask` | Ask the caller a question mid-run. |
| `agent.spawn` | Run a sub-agent and get its result back, under an attenuated capability set -- a spawned agent can never end up with MORE authority than the code that spawned it. |

## Filter and summarize in-process -- do not round-trip large results through the model

The single most valuable CodeAct pattern: when a step produces a result too large to usefully show
the model whole (a big table from `agent.data`, a long tool result from `agent.tools`), keep working
on it in the SAME `execute_code` call or a following one, and only surface the part that actually
answers the task -- a filtered subset, an aggregate, a short summary. Bringing the whole intermediate
result back to the model just so it can decide what to do with it wastes the run's token budget on
data the model was never going to read in full anyway; doing the filtering in Python is strictly
cheaper and exact, not just faster.

## Multi-step is for real dependencies, not habit

Reach for several `execute_code` calls in sequence only when a later step genuinely depends on a
result from an earlier one that is cheaper to inspect than to predict up front (an unknown schema,
an unknown row count). A task with no such dependency is one call, not several -- see
using-the-code-interpreter for that boundary.
)SKILL";

inline constexpr std::string_view kReadingLargeContentSkillMd = R"SKILL(---
name: reading-large-content
description: When a tool result or file is too large for the run's remaining token budget, it arrives as a reference you open and page through explicitly instead of prose already in the transcript. Use this before asking for a whole file or a large tool result verbatim.
metadata:
  version: "1"
---
# Reading large content

Every tool result is size-bounded against the run's ACTUAL remaining token budget, not a fixed byte
constant -- the same result that fits comfortably against a large-context model may not fit at all
against a small one, and the threshold is computed per call from what the turn can actually afford
before the result is even appended to the transcript.

## What happens above the threshold

A result that would exceed its budgeted share does not get silently truncated into something
misleading, and it does not fail the call. It arrives as a reference instead of inline content --
the same content-goes-to-a-handle pattern used for bulk structured data, generalized to any
oversized tool result. Treat that reference the way you would treat a large file you already know is
large: open it explicitly and page through only the part relevant to the task, rather than asking for
the whole thing rendered into the conversation at once.

## Preview before you page

When you are not yet sure which part of a large result matters, read a small preview first -- the
first page, a summary, a schema -- and decide what to page in next FROM that preview, rather than
requesting the full content up front on the chance some of it is useful. This mirrors how you would
work with a large file on a real filesystem: `head` before `cat`.

## Why this matters for the run, not just this one call

A single oversized first tool call -- reading one very large file, say -- is exactly the failure mode
this guards against: consuming the run's entire remaining budget in one shot, before the model gets a
chance to do anything else with it. Paging deliberately, a bounded amount at a time, keeps every
later step in the run affordable too, not just the current one.
)SKILL";

inline constexpr std::string_view kProducingStructuredOutputSkillMd = R"SKILL(---
name: producing-structured-output
description: Shaping a run's final response against a declared output schema reliably, using whichever of the three enforcement strategies (native, tool-shaped, parse-and-repair) the current provider actually supports. Use this when the run declares an output schema and the final answer must validate against it.
metadata:
  version: "1"
---
# Producing structured output

When a run declares a typed output schema, the final response must validate against it -- JSON
Schema 2020-12, the same schema dialect tool arguments already use, so there is exactly one validator
in play, not two. How that gets enforced depends on what the current provider actually supports; you
do not need to pick a strategy yourself, but understanding the three in preference order explains why
a well-formed response still sometimes gets rejected and re-asked for.

## Three strategies, in preference order

1. **Native** -- the provider itself enforces the JSON schema via constrained decoding. Preferred
   whenever the provider supports it: the output is correct by construction, not checked after the
   fact.
2. **Tool-shaped** -- when native enforcement isn't available, the schema is presented as a single
   forced tool call instead of free-text output. Answer it exactly like any other tool call, with
   arguments that satisfy the declared schema.
3. **Parse-and-repair** -- the last resort, used only when neither of the above is available: the
   response is parsed as free text and, on a validation failure, the run gets one bounded re-ask
   rather than failing outright. A high repair rate here is a signal of a genuine prompt or provider
   mismatch worth noticing, not a normal path to rely on.

## What this means for how you answer

Produce output that matches the declared schema's shape -- required fields present, types correct,
no extra prose wrapped around it when the strategy in play is native or tool-shaped. If you are
unsure a value satisfies a constraint the schema states (an enum, a format, a bound), get it right the
first time rather than assuming a repair pass will catch it -- repair is bounded, not guaranteed, and
strategy 3 is not always available.
)SKILL";

inline constexpr std::string_view kShellPipelinesSkillMd = R"SKILL(---
name: shell-pipelines
description: ShellRunner's small, explicitly non-POSIX-complete grammar -- pipes, redirects, variable assignment/expansion, && / ||, and minimal control flow -- plus its fixed builtin set and how any other command name resolves through the ordinary tool pipeline. Use this before writing a shell command in this environment.
metadata:
  version: "1"
---
# Shell pipelines

The shell tool in this environment is `ShellRunner`, engine-native code that parses a real but
deliberately SMALL grammar -- it is not a wrapped system shell, and it does not claim POSIX
conformance. Stay within the documented subset rather than assuming a POSIX shell feature will work.

## What the grammar covers

- Pipes (`|`) and redirects.
- Variable assignment and expansion.
- `&&` and `||` for conditional sequencing.
- Minimal control flow.

That is the whole surface. A construct outside this list is not a bug to route around with
cleverness -- it simply is not part of the grammar; do the equivalent work in the code interpreter
(see using-the-code-interpreter) instead of fighting the shell grammar to express it.

## Builtins vs. everything else

A fixed set of ordinary builtins -- `cd`, `pwd`, `ls`, `cat`, `echo`, `export`, `mkdir`, `rm`, `mv`,
`cp`, and similar -- is implemented directly against the mounted worktree and the capability layer.
Every other command name resolves ONLY to a registered Runner or a registered Tool -- there is no
search-path lookup, no arbitrary binary execution. A name that is neither a builtin nor something
registered produces the same "command not found" a real shell would show, but for a structurally
different reason: no capability or registration exists for it, not "not found in PATH."

## The same tool, reached two ways

A command like `grep` in a shell pipeline and the equivalent call through `agent.tools` in Python
(see using-codeact) resolve to the exact same registered Tool, through the exact same tool pipeline --
validation, authorization, the works. Do not expect different behavior depending on which frontend
you reach a tool from; if one works a certain way, the other does too.
)SKILL";

namespace builtin_skills_detail {

// Shared by all five constants above: parse against the skill's own declared name (used as the
// "directory name" even though there is no real directory -- `parse_skill_md`'s own
// `expected_directory_name` check is exactly the self-consistency check we want here too, "does the
// name field match what this constant claims to be") and wrap the source text itself as the skill's
// one bundle file, byte-for-byte, the same as a real `SKILL.md` on disk would be.
[[nodiscard]] inline result<SkillSourceResult> parse_builtin(std::string_view name,
                                                               std::string_view md_text) {
    auto skill = parse_skill_md(md_text, name);
    if (!skill) return std::unexpected(skill.error());

    std::string const text(md_text);
    std::vector<std::byte> bytes(reinterpret_cast<std::byte const*>(text.data()),
                                  reinterpret_cast<std::byte const*>(text.data()) + text.size());
    std::vector<SkillBundleFile> files;
    files.push_back(SkillBundleFile{"SKILL.md", std::move(bytes)});
    return SkillSourceResult{std::move(*skill), std::move(files)};
}

}  // namespace builtin_skills_detail

// All five of §8f's generic skills, real content, parsed at construction time exactly like a real
// disk-sourced skill would be, just without disk I/O.
//
// Parsed EAGERLY, once, here -- not lazily inside a returned closure -- so a malformed constant
// (a bug in one of the raw string literals above, never a runtime condition) is caught the first
// time this function is called, in whichever test exercises it, rather than being silently
// swallowed into an empty or partial skill list. A parse failure is carried into the returned
// `load_skills` closure and surfaces there exactly like any other source's real failure would --
// `SkillsProvider::resolve_and_mount` (skill_provider.hpp) already fails the WHOLE `on_context()`
// call closed on any one source's failure, which is the correct, consistent place for this to be
// observed, not swallowed here.
[[nodiscard]] inline SkillSourceDescriptor make_builtin_skills_source() {
    result<std::vector<SkillSourceResult>> resolved = [] {
        std::vector<SkillSourceResult> skills;
        struct Entry {
            std::string_view name;
            std::string_view text;
        };
        Entry const entries[] = {
            {"using-the-code-interpreter", kUsingTheCodeInterpreterSkillMd},
            {"using-codeact", kUsingCodeactSkillMd},
            {"reading-large-content", kReadingLargeContentSkillMd},
            {"producing-structured-output", kProducingStructuredOutputSkillMd},
            {"shell-pipelines", kShellPipelinesSkillMd},
        };
        skills.reserve(std::size(entries));
        for (auto const& entry : entries) {
            auto parsed = builtin_skills_detail::parse_builtin(entry.name, entry.text);
            if (!parsed) return result<std::vector<SkillSourceResult>>(std::unexpected(parsed.error()));
            skills.push_back(std::move(*parsed));
        }
        return result<std::vector<SkillSourceResult>>(std::move(skills));
    }();

    SkillSourceDescriptor d;
    d.origin_id = "builtin";
    d.load_skills = [resolved]() { return resolved; };
    return d;
}

}  // namespace agentengine
