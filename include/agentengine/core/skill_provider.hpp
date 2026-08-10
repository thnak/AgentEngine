#pragma once
// Implements 009-Plugin-and-Extension-System.md §8b-8c -- the `ContextProvider` conformer that
// actually mounts resolved skills (core/worktree.hpp's real Tree/Ref/Mount machinery) read-only at
// `/skills/<name>` and contributes their ~100-token "name: description" advertisement to the model's
// context. Never introduces `load_skill`/`read_skill_resource` tool wrappers (§8b's own explicit,
// deliberate divergence from MAF) -- the mount IS the mechanism; the agent reads a skill with
// ordinary file operations (026 §6) via `mount_read`, same as any other mounted content.
//
// Mount path: FLAT `/skills/<name>`, matching §8b's literal text exactly. §8c's own anti-shadowing
// requirement ("a skill fetched from a remote source can never shadow a local one") is satisfied by
// REJECTING at load time when two declared sources would produce the same skill name -- a fail-
// closed refusal to mount either, not a silent last-source-wins, and not a namespaced path scheme
// that would deviate from §8b's text. See 009 §8c's own added clarifying paragraph for why this is a
// documentation note, not a design change.

#include <algorithm>
#include <string>
#include <vector>

#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/reference_agent_prompt.hpp"
#include "agentengine/core/skill_source.hpp"
#include "agentengine/core/worktree.hpp"
#include "quark/core/persistence.hpp"

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

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext&, EffectContext&) {
        if (!loaded_) {
            load_error_ = resolve_and_mount();
            loaded_ = true;
        }

        ContextContribution contribution;
        if (!load_error_) co_return std::unexpected(load_error_.error());

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
    // back out (e.g. a test, or a real agent-facing file-read surface once one exists) -- `mounted()`
    // is only meaningful after at least one `on_context()` call; empty before that or on load failure.
    [[nodiscard]] std::vector<Mount> const& mounted() const noexcept { return mounts_; }
    [[nodiscard]] ObjectStoreT& object_store() noexcept { return object_store_; }
    [[nodiscard]] quark::InMemoryStore& ref_store() noexcept { return ref_store_; }

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

                auto committed_ref = commit_ref(ref_store_, "skill:" + name, *tree_digest);
                if (!committed_ref) return std::unexpected(committed_ref.error());

                pending_mounts.push_back(Mount{"/skills/" + name, committed_ref->name, ""});
                pending_summaries.push_back(PromptSkillSummary{name, item.skill.frontmatter.description});
            }
        }

        mounts_ = std::move(pending_mounts);
        summaries_ = std::move(pending_summaries);
        return {};
    }

    std::vector<SkillSourceDescriptor> sources_;
    ObjectStoreT object_store_;
    quark::InMemoryStore ref_store_;
    bool loaded_ = false;
    result<void> load_error_ = result<void>{};
    std::vector<Mount> mounts_;
    std::vector<PromptSkillSummary> summaries_;
};

static_assert(ContextProvider<SkillsProvider<>>,
              "SkillsProvider must satisfy the ContextProvider concept (005 §5) to occupy "
              "AgentSession's HistoryProviderT slot, directly or via HistoryAndSkillsProvider");

}  // namespace agentengine
