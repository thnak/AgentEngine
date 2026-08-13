#pragma once
// Implements 009-Plugin-and-Extension-System.md §8b-8c -- the `ContextProvider` conformer that
// actually mounts resolved skills (core/worktree.hpp's real Tree/Ref/Mount machinery) read-only,
// logically at `/skills/<name>`, and contributes their ~100-token "name: description" advertisement
// to the model's context. Never introduces `load_skill`/`read_skill_resource` tool wrappers (§8b's
// own explicit, deliberate divergence from MAF) -- the mount IS the mechanism; the agent reads a
// skill with ordinary file operations (026 §6) via `mount_read`, same as any other mounted content.
//
// Mount identity: each mounted skill's `Mount.mount_id` is the BARE skill name (e.g. "using-codeact"),
// not the literal string "/skills/<name>". `/skills/<name>` (§8b's literal text) is the skill's
// LOGICAL location -- carried to the model via the advertisement message/`PromptSkillSummary` below,
// unaffected by this choice -- while `mount_id` is purely the capability-matching key `cap::FsRead`/
// `cap::FsWrite` compare against (`worktree.hpp`'s `mount_read`/`mount_write`: exact string equality,
// no path semantics). A bare name matches the single-path-segment mount-id convention every native_jail
// sandbox root already uses ("work", "input", "out" -- `mediated_python_runner.cpp`'s guest-path
// resolution takes the FIRST path segment as the mount id), which a mount_id containing embedded `/`
// never would have; it also gives genuine PER-SKILL capability granularity (an operator can grant
// `cap::FsRead{"using-codeact", ...}` without exposing every other mounted skill), never possible with
// one shared mount_id. §8c's own anti-shadowing requirement ("a skill fetched from a remote source can
// never shadow a local one") is satisfied by REJECTING at load time when two declared sources would
// produce the same skill name -- a fail-closed refusal to mount either, not a silent last-source-wins.
//
// Tool scoping: `allowed_tool_names()` (below) exposes the deduplicated union of every currently
// resolved skill's `allowed-tools` frontmatter field -- consumed by `core/skill_tool_scoping.hpp` to
// restrict which tools are BOTH declared to the model and actually invocable this run (009 §8c: no
// longer merely advisory once wired through that header -- see its own top comment for the real
// enforcement boundary this depends on).
//
// Skills Phase 3 (on-demand mounting, decisions/ADR-024's addendum): "resolved" (this class's own
// scope -- every configured skill is resolved and its files are materialized unconditionally, per
// §8b) is a DIFFERENT thing from "mounted" (an agent-triggered SUBSET, tracked outside this class --
// see `tools/cli_chat.cpp`'s `MountedSkillsState`). `allowed_tool_names_for(mounted_names)` and
// `body_of(name)` below exist so a caller with its own notion of "currently mounted" can compute a
// NARROWER tool-scoping/context-injection than "everything resolved" without this class needing to
// know anything about mount state itself -- it only ever answers "what does skill X declare," never
// "is skill X currently active."

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/reference_agent_prompt.hpp"
#include "agentengine/core/skill_source.hpp"
#include "agentengine/core/worktree.hpp"
#include "agentengine/rt/append_log_store.hpp"

namespace agentengine {

namespace skill_provider_detail {

// One (segments, blob_digest) pair, resolved from a `SkillBundleFile`'s `relative_path` -- kept
// separate from the file's own bytes so `assemble_tree` (below) can group/recurse over paths
// without re-touching the (already stored) blob content.
struct PendingTreeEntry {
    std::vector<std::string> segments;
    Digest blob_digest;
};

// Bottom-up Tree assembly from a flat list of (path segments, blob digest) pairs -- the local
// equivalent of `worktree.hpp`'s own `set_entry_at_path`, but building a BRAND NEW tree from
// scratch in one pass (a skill bundle's files are all known up front) rather than patching one leaf
// at a time into an existing committed tree, which is what that function is for instead. Groups
// entries by their first path segment: a file whose path is exactly one segment becomes a direct
// blob `TreeEntry`; everything else is grouped by its leading segment, stripped of that segment, and
// recursed into a subtree.
template <WorktreeObjectStore ObjectStoreT>
[[nodiscard]] result<Digest> assemble_tree(ObjectStoreT& store, std::vector<PendingTreeEntry> entries) {
    std::vector<TreeEntry> tree_entries;
    std::vector<std::pair<std::string, std::vector<PendingTreeEntry>>> groups;

    for (auto& entry : entries) {
        if (entry.segments.empty()) continue;  // unreachable: split_mount_path never returns an empty
                                                 // non-root segment list for a non-empty relative_path
        if (entry.segments.size() == 1) {
            tree_entries.push_back(TreeEntry{entry.segments.front(), entry.blob_digest, false});
            continue;
        }
        std::string const head = entry.segments.front();
        auto it = std::ranges::find_if(groups, [&](auto const& g) { return g.first == head; });
        if (it == groups.end()) {
            groups.emplace_back(head, std::vector<PendingTreeEntry>{});
            it = groups.end() - 1;
        }
        std::vector<std::string> rest(entry.segments.begin() + 1, entry.segments.end());
        it->second.push_back(PendingTreeEntry{std::move(rest), entry.blob_digest});
    }

    for (auto& [name, sub_entries] : groups) {
        auto sub_digest = assemble_tree(store, std::move(sub_entries));
        if (!sub_digest) return std::unexpected(sub_digest.error());
        tree_entries.push_back(TreeEntry{name, *sub_digest, true});
    }

    return store.put_tree(Tree{std::move(tree_entries)});
}

}  // namespace skill_provider_detail

// Resolves every declared `SkillSourceDescriptor`, mounts each resolved skill read-only at
// `/skills/<name>`, and contributes one `role::system` advertisement `Message` naming every mounted
// skill. Owns its own object/ref store -- a complete, self-contained `ContextProvider` conformer that
// needs no wiring into `AgentSession` beyond occupying its existing single `HistoryProviderT` slot
// (directly, or composed via `history_and_skills_provider.hpp`).
template <WorktreeObjectStore ObjectStoreT = InMemoryWorktreeObjectStore>
class SkillsProvider {
public:
    explicit SkillsProvider(std::vector<SkillSourceDescriptor> sources) : sources_(std::move(sources)) {}

    // Forces resolution outside a coroutine context -- `resolve_and_mount()` is already fully
    // synchronous internally (no internal `co_await`); `on_context`'s `task<>` wrapper exists only to
    // satisfy `ContextProvider`, not because resolution itself needs one. Lets a plain synchronous
    // caller (the native_jail materializer, a CLI's startup sequence) force resolve-once/freeze
    // without standing up a coroutine driver just to reach it.
    [[nodiscard]] result<void> ensure_loaded() {
        if (!loaded_) {
            load_error_ = resolve_and_mount();
            loaded_ = true;
        }
        return load_error_;
    }

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext&, EffectContext&) {
        if (auto r = ensure_loaded(); !r) co_return std::unexpected(r.error());

        ContextContribution contribution;
        if (!summaries_.empty()) {
            std::string text;
            for (auto const& s : summaries_) text += s.name + ": " + s.description + "\n";
            Message advertisement;
            advertisement.role = role::system;
            ContentItem item;
            item.origin = content_origin::system;
            item.value = Text{std::move(text)};
            advertisement.content.push_back(std::move(item));
            contribution.messages.push_back(std::move(advertisement));
        }
        co_return contribution;
    }

    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }

    // Introspection for a caller that needs to grant `cap::FsRead`/actually read a mounted skill
    // back out (a test, or `native_jail::materialize_skill_mounts`) -- `mounted()` is only meaningful
    // after at least one `on_context()`/`ensure_loaded()` call; empty before that or on load failure.
    [[nodiscard]] std::vector<Mount> const& mounted() const noexcept { return mounts_; }
    [[nodiscard]] ObjectStoreT& object_store() noexcept { return object_store_; }
    [[nodiscard]] rt::InMemoryAppendLogStore& ref_store() noexcept { return *ref_store_; }

    // Deduplicated union of every currently RESOLVED skill's `allowed-tools` frontmatter field -- same
    // resolve-once/freeze lifecycle and empty-before/on-failure contract as `mounted()`. This is the
    // FULL universe a run could ever unlock, not what's currently active -- see `allowed_tool_names_for`
    // below for the mount-scoped narrowing.
    [[nodiscard]] std::vector<std::string> const& allowed_tool_names() const noexcept {
        return allowed_tool_names_;
    }

    // Deduplicated union of `allowed-tools` restricted to `mounted_names` -- skill names not present in
    // `per_skill_allowed_tools_` (unresolved, or resolution failed) are silently skipped, matching this
    // class's own "reject rather than guess" posture applied the other way: an invalid name here isn't
    // this QUERY's job to validate (that already happened, or should happen, at the mount-recording call
    // site -- `tools/cli_chat.cpp`'s `MountSkillTool`).
    [[nodiscard]] std::vector<std::string> allowed_tool_names_for(
        std::vector<std::string> const& mounted_names) const {
        std::vector<std::string> out;
        for (auto const& name : mounted_names) {
            auto it = per_skill_allowed_tools_.find(name);
            if (it == per_skill_allowed_tools_.end()) continue;
            for (auto const& tool_name : it->second) {
                if (std::ranges::find(out, tool_name) == out.end()) out.push_back(tool_name);
            }
        }
        return out;
    }

    // The full Markdown body (everything after the closing `---`) of a resolved skill, by name -- what
    // `tools/cli_chat.cpp` injects as an additional system message once a skill is mounted, so the
    // agent doesn't need a fresh `open()` call to see it again on a later turn. `std::nullopt` for an
    // unresolved name.
    [[nodiscard]] std::optional<std::string> body_of(std::string const& name) const {
        auto it = body_by_name_.find(name);
        if (it == body_by_name_.end()) return std::nullopt;
        return it->second;
    }

private:
    [[nodiscard]] result<void> resolve_and_mount() {
        // Built entirely into LOCAL vectors and only assigned to `mounts_`/`summaries_` at the very
        // end, on total success -- a mid-loop collision or a later source's own failure must leave
        // the externally-observable state (`mounted()`, and the advertisement message built from
        // `summaries_`) exactly as it was before this call, never a partial set from whichever
        // skills happened to process before the failure. (Object-store/ref-store writes for skills
        // processed before a later failure are a harmless, orphaned, content-addressed side effect --
        // never referenced by any Mount this provider will ever hand out -- not a correctness
        // problem this function needs to roll back.)
        std::vector<std::string> claimed_names;
        std::vector<std::string> claimed_origins;
        std::vector<Mount> pending_mounts;
        std::vector<PromptSkillSummary> pending_summaries;
        std::vector<std::string> pending_allowed_tools;
        std::unordered_map<std::string, std::vector<std::string>> pending_per_skill_allowed_tools;
        std::unordered_map<std::string, std::string> pending_body_by_name;

        for (auto const& source : sources_) {
            auto resolved = source.load_skills();
            if (!resolved) return std::unexpected(resolved.error());

            for (auto& item : *resolved) {
                std::string const& name = item.skill.frontmatter.name;
                for (std::size_t i = 0; i < claimed_names.size(); ++i) {
                    if (claimed_names[i] == name) {
                        return std::unexpected(error{
                            failure_class::contract,
                            "skill '" + name + "' is declared by both '" + claimed_origins[i] +
                                "' and '" + source.origin_id + "' -- a skill from one source must "
                                "never shadow a skill from another (009 §8c)",
                            "skill.name_collision_across_sources"});
                    }
                }
                claimed_names.push_back(name);
                claimed_origins.push_back(source.origin_id);

                std::vector<skill_provider_detail::PendingTreeEntry> pending;
                pending.reserve(item.files.size());
                for (auto const& file : item.files) {
                    auto blob_digest = object_store_.put_blob(file.bytes);
                    if (!blob_digest) return std::unexpected(blob_digest.error());
                    auto segments = split_mount_path(file.relative_path);
                    if (!segments) return std::unexpected(segments.error());
                    pending.push_back(
                        skill_provider_detail::PendingTreeEntry{std::move(*segments), std::move(*blob_digest)});
                }
                auto tree_digest = skill_provider_detail::assemble_tree(object_store_, std::move(pending));
                if (!tree_digest) return std::unexpected(tree_digest.error());

                auto committed_ref = commit_ref(*ref_store_, "skill:" + name, *tree_digest);
                if (!committed_ref) return std::unexpected(committed_ref.error());

                // Bare skill name, not "/skills/" + name -- see this file's own top comment for why
                // mount_id is a capability-matching key, distinct from the skill's logical path.
                pending_mounts.push_back(Mount{name, committed_ref->name, ""});
                pending_summaries.push_back(PromptSkillSummary{name, item.skill.frontmatter.description});
                pending_per_skill_allowed_tools[name] = item.skill.frontmatter.allowed_tools;
                pending_body_by_name[name] = item.skill.body;
                for (auto const& tool_name : item.skill.frontmatter.allowed_tools) {
                    if (std::ranges::find(pending_allowed_tools, tool_name) == pending_allowed_tools.end()) {
                        pending_allowed_tools.push_back(tool_name);
                    }
                }
            }
        }

        mounts_ = std::move(pending_mounts);
        summaries_ = std::move(pending_summaries);
        allowed_tool_names_ = std::move(pending_allowed_tools);
        per_skill_allowed_tools_ = std::move(pending_per_skill_allowed_tools);
        body_by_name_ = std::move(pending_body_by_name);
        return {};
    }

    std::vector<SkillSourceDescriptor> sources_;
    ObjectStoreT object_store_;
    // Held via shared_ptr, not by value: `rt::InMemoryAppendLogStore` owns a `std::mutex` (its own
    // internal locking), which makes it move-only by default -- but `SkillsProvider` itself must stay
    // COPY-constructible, because `HistoryAndSkillsProvider`'s `make_context_provider_descriptor`
    // stores it inside a `std::function`-based type-erased wrapper, and `std::function` requires its
    // target to be CopyConstructible to be stored at all (even though this codebase's own call site
    // only ever MOVES a SkillsProvider into that wrapper once, never actually copies it at runtime).
    // The indirection is behaviorally inert -- there is exactly one SkillsProvider instance per
    // session either way -- and keeps `rt::InMemoryAppendLogStore` itself unchanged rather than
    // bolting a lock-and-copy constructor onto a primitive other callers rely on staying simple.
    std::shared_ptr<rt::InMemoryAppendLogStore> ref_store_ = std::make_shared<rt::InMemoryAppendLogStore>();
    bool loaded_ = false;
    result<void> load_error_ = result<void>{};
    std::vector<Mount> mounts_;
    std::vector<PromptSkillSummary> summaries_;
    std::vector<std::string> allowed_tool_names_;
    std::unordered_map<std::string, std::vector<std::string>> per_skill_allowed_tools_;
    std::unordered_map<std::string, std::string> body_by_name_;
};

static_assert(ContextProvider<SkillsProvider<>>,
              "SkillsProvider must satisfy the ContextProvider concept (005 §5) to occupy "
              "AgentSession's HistoryProviderT slot, directly or via HistoryAndSkillsProvider");

}  // namespace agentengine
