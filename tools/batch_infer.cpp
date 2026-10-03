// Standalone, non-interactive batch-inference CLI: sends N independent single-shot prompts to OpenRouter's Batch
// API as one job, polls it to the end, and prints each answer. decisions/ADR-235 §7 / ADR-236 §4: it drives the
// engine's own `openai::OpenRouterBatchBackend` (GA `/api/v1/batches`), so the tool and a batched workflow round
// speak the vendor through ONE wire translation -- the tool is a manual live probe of exactly the code the engine
// runs, not a second client that could drift from it.
//
// What it does NOT do: no WorkflowSupervisor, no AgentSession. It calls the `BatchBackend` seam directly
// (admit -> submit -> poll -> release), which is the whole vendor surface ADR-235 batches through.
//
// Earlier versions of this file spoke the beta `/api/beta/batches` with their own request builder, and submitted a
// second job in the Anthropic `/v1/messages` shape. That second job is gone with the hand-written client: the
// engine's backend speaks the chat-completions shape only (ADR-235 §3.2). OpenRouter's GA endpoint does accept a
// `/v1/messages` job (probed 2026-10-03, ADR-236 §4); speaking it from the engine would be a second backend shape,
// not a change to this tool.
//
// Model: `google/gemini-2.5-flash-lite` by default -- accepted by the GA endpoint on 2026-10-02. Batch support is
// per model AND per account: `openai/gpt-4o-mini`, which an earlier version of this comment called "confirmed
// batch-capable" (true of the beta on 2026-08-21), was refused 400 "does not have a :batch endpoint" for this
// account on the GA endpoint (docs/research/2026-10-02-batch-inference-provider-limits.md).
//
// Environment:
//   AGENTENGINE_OPENROUTER_BATCH_API_KEY  the key; falls back to AGENTENGINE_OPENROUTER_API_KEY. Read at run time,
//                                         never compiled in (018 §4).
//   AGENTENGINE_OPENROUTER_BATCH_MODEL    optional model override.
//   AGENTENGINE_OPENROUTER_BATCH_WAIT_S   optional poll budget in seconds (default 900). Running out is a wait cap,
//                                         not a verdict: the vendor window is 24h.
//   AGENTENGINE_BATCH_RECORDING           optional path. Every backend call is appended there as JSON Lines through
//                                         `BatchRecorder` (core/batch_recording.hpp, ADR-236), replayable offline.
//                                         Refused if the file already holds data: one file is one run.
//
// Usage: agentengine_batch_infer [prompt ...]   (three short built-in prompts when none are given)

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/core/batch_recording.hpp"
#include "agentengine/pal/console.hpp"
#include "agentengine/pal/env.hpp"
#include "agentengine/protocol/openai/openrouter_batch_backend.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"

using namespace agentengine;

namespace {

constexpr char const* kDefaultModel = "google/gemini-2.5-flash-lite";
constexpr char const* kHost         = "openrouter.ai";
constexpr char const* kSecretName   = "openrouter";
// A job is invisible to GET for a few seconds after a successful submit (measured live: 404 at +5 s, found at
// +15 s). Poll errors inside this window are retried; past it the tool gives up only after kMaxPollErrors
// consecutive non-transient ones. Both are the engine's own `BatchPolicy` defaults.
constexpr std::chrono::minutes kPollErrorGrace{2};
constexpr int                  kMaxPollErrors = 5;

[[nodiscard]] std::optional<std::string> env_nonempty(char const* name) {
    auto v = pal::env_var(name);
    if (v && !v->empty()) return v;
    return std::nullopt;
}

[[nodiscard]] Message user_text(std::string text) {
    ContentItem item;
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m;
    m.role = role::user;
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] std::string text_of(Message const& m) {
    std::string out;
    for (ContentItem const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) out += t->text;
    }
    return out;
}

void print_poll(BatchPoll const& poll, std::vector<BatchItemRequest> const& items) {
    for (BatchItemRequest const& sent : items) {
        auto const it = std::find_if(poll.items.begin(), poll.items.end(),
                                     [&](BatchItemResult const& r) { return r.custom_id == sent.custom_id; });
        if (it == poll.items.end()) {
            std::fprintf(stderr, "  %s -> no result (the job ended without one)\n", sent.custom_id.c_str());
        } else if (it->status != batch_item_status::succeeded) {
            std::fprintf(stderr, "  %s -> %s: %s\n", sent.custom_id.c_str(), batch_item_status_tag(it->status),
                         it->detail.c_str());
        } else {
            std::fprintf(stderr, "  %s -> \"%s\" (in=%llu out=%llu)\n", sent.custom_id.c_str(),
                         text_of(it->response.message).c_str(),
                         static_cast<unsigned long long>(it->response.usage.input_tokens),
                         static_cast<unsigned long long>(it->response.usage.output_tokens));
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    // GitHub issue #48: a Windows console left on its OEM code page renders a model's non-ASCII reply as mojibake.
    // First statement, because the console decodes when bytes reach it, not when they are buffered.
    pal::ConsoleUtf8Scope console_utf8;

    auto key = env_nonempty("AGENTENGINE_OPENROUTER_BATCH_API_KEY");
    if (!key) key = env_nonempty("AGENTENGINE_OPENROUTER_API_KEY");
    if (!key) {
        std::fprintf(stderr, "Set AGENTENGINE_OPENROUTER_BATCH_API_KEY (or AGENTENGINE_OPENROUTER_API_KEY) to an "
                             "OpenRouter key and re-run.\n");
        return 1;
    }
    std::string const model = env_nonempty("AGENTENGINE_OPENROUTER_BATCH_MODEL").value_or(kDefaultModel);
    std::string const wait_text = env_nonempty("AGENTENGINE_OPENROUTER_BATCH_WAIT_S").value_or("900");
    int wait_s = 0;
    if (auto const [end, ec] = std::from_chars(wait_text.data(), wait_text.data() + wait_text.size(), wait_s);
        ec != std::errc{} || end != wait_text.data() + wait_text.size() || wait_s <= 0) {
        std::fprintf(stderr, "AGENTENGINE_OPENROUTER_BATCH_WAIT_S must be a positive number of seconds, got '%s'\n",
                     wait_text.c_str());
        return 1;
    }

    std::vector<std::string> prompts;
    for (int i = 1; i < argc; ++i) prompts.emplace_back(argv[i]);
    if (prompts.empty()) {
        prompts = {"Name the capital of France in one word.", "What is 2 + 2? Answer with just the number.",
                   "Name one primary color."};
    }

    InMemorySecretStore store;
    store.set(kSecretName, *key);
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{kSecretName, std::chrono::seconds{0}}});
    EffectContext ctx;
    ctx.principal    = Principal{"batch-infer", ""};
    ctx.capabilities = borrow_capabilities(held);

    std::shared_ptr<BatchBackend> backend = std::make_shared<openai::OpenRouterBatchBackend<InMemorySecretStore>>(
        kHost, std::uint16_t{443}, model, SecretRef{kSecretName}, store);
    auto const recording = env_nonempty("AGENTENGINE_BATCH_RECORDING");
    std::optional<BatchRecorder> recorder;
    if (recording) {
        std::error_code ec;
        if (std::filesystem::exists(*recording, ec) && std::filesystem::file_size(*recording, ec) > 0) {
            std::fprintf(stderr, "AGENTENGINE_BATCH_RECORDING names a file that already holds a recording (%s); use a "
                                 "new path -- one file is one run.\n", recording->c_str());
            return 1;
        }
        recorder.emplace([path = *recording](BatchCallRecord const& r) {
            if (auto ok = append_batch_call_record(path, r); !ok) {
                std::fprintf(stderr, "  recording write failed: %s\n", ok.error().message.c_str());
            }
        });
        backend = recorder->wrap(backend);
    }

    std::fprintf(stderr, "AgentEngine batch inference -- %s model=%s prompts=%zu%s%s\n", backend->group_key().c_str(),
                 model.c_str(), prompts.size(), recording ? " recording=" : "", recording ? recording->c_str() : "");

    std::vector<BatchItemRequest> items;
    for (std::size_t i = 0; i < prompts.size(); ++i) {
        ChatRequest req;
        req.messages.push_back(user_text(prompts[i]));
        if (auto admitted = backend->admit(req); !admitted) {
            std::fprintf(stderr, "prompt %zu is not batchable: %s\n", i, admitted.error().message.c_str());
            return 1;
        }
        items.push_back(BatchItemRequest{"i" + std::to_string(i), std::move(req)});
    }

    auto job = backend->submit(items, ctx);
    if (!job) {
        std::fprintf(stderr, "submit failed: %s (%s)\n", job.error().message.c_str(), job.error().code.c_str());
        return 1;
    }
    std::fprintf(stderr, "submitted: %s\n", job->c_str());

    auto const submitted_at = std::chrono::steady_clock::now();
    auto const deadline     = submitted_at + std::chrono::seconds(wait_s);
    std::chrono::seconds backoff{5};
    int consecutive_errors = 0;
    for (;;) {
        std::this_thread::sleep_for(backoff);
        backoff = std::min(backoff * 2, std::chrono::seconds{30});
        auto poll = backend->poll(*job, ctx);
        if (!poll) {
            bool const young   = std::chrono::steady_clock::now() - submitted_at < kPollErrorGrace;
            bool const counted = !young && poll.error().klass != failure_class::transient;
            if (counted) ++consecutive_errors;
            std::fprintf(stderr, "  poll failed%s: %s\n",
                         young ? " (inside the post-submit grace, retrying)" : counted ? " (counted)" : " (transient)",
                         poll.error().message.c_str());
            if (consecutive_errors >= kMaxPollErrors) {
                std::fprintf(stderr, "batch_infer: %d consecutive poll failures; giving up. The job may still be live "
                                     "and billed: %s\n", kMaxPollErrors, job->c_str());
                return 1;
            }
        } else if (poll->ended) {
            std::fprintf(stderr, "job ended%s%s\n", poll->detail.empty() ? "" : ": ", poll->detail.c_str());
            print_poll(*poll, items);
            if (auto released = backend->release(*job, ctx); !released) {
                std::fprintf(stderr, "  release failed (the vendor keeps the job's data): %s\n",
                             released.error().message.c_str());
            }
            // Every custom id sent has exactly one succeeded result -- a duplicate cannot stand in for a missing one.
            bool const all_ok = std::all_of(items.begin(), items.end(), [&](BatchItemRequest const& sent) {
                return std::count_if(poll->items.begin(), poll->items.end(), [&](BatchItemResult const& r) {
                           return r.custom_id == sent.custom_id && r.status == batch_item_status::succeeded;
                       }) == 1;
            });
            if (recorder && recorder->failed()) {
                std::fprintf(stderr, "batch_infer: the recording is incomplete (a write failed) and will not replay "
                                     "exactly\n");
            }
            std::fprintf(stderr, "\nbatch_infer: %s\n", all_ok ? "OK" : "FAIL");
            return all_ok ? 0 : 1;
        } else {
            consecutive_errors = 0;
            std::fprintf(stderr, "  in progress\n");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "batch_infer: still pending after %ds -- a wait cap, not a verdict (vendor window "
                                 "24h). The job keeps running; OpenRouter has no cancel endpoint.\n", wait_s);
            return 2;
        }
    }
}
