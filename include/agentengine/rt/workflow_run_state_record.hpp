#pragma once
// Implements 014-Workflow-and-Orchestration.md §5 -- the ON-DISK SHAPE of a workflow checkpoint:
// `RunStateRecord` and its parts (`DeliveryRecord`, `ExecutorOutputRecord`, `HeldFanInRecord`,
// `OpenPortRecord`), their JSON codec, and `encode_run_state_record()`/`decode_run_state_record()`.
//
// Moved verbatim out of `rt/workflow_supervisor.hpp`, which includes this header, so every existing
// includer compiles unchanged. `WorkflowSupervisor::to_record()`/`restore_from_record()` (which map
// the supervisor's live state onto these types) stay in that file: they read member state, and none
// of this does -- every declaration here sat before `class WorkflowSupervisor` and could not touch it.
// See that file's banner (SLICE 2) for the checkpointing design and its disclosed narrowings
// (single-slot, latest-only).
//
// Why a file of its own. The checkpoint format is a durable artifact: it outlives the process that
// wrote it, so it needs round-trip and compatibility tests that do not stand up a supervisor and run
// a workflow first. And one consumer only ever wanted this half -- `rt/workflow_time_travel.hpp`
// references the record types fifteen times and `WorkflowSupervisor` not once, yet compiled the
// whole ~2900-line supervisor to get them. It includes this header now.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/interaction.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/rt/interaction_codec.hpp"
#include "agentengine/rt/message_codec.hpp"

namespace agentengine::rt {

// ae-naming-lint: allow DeliveryRecord — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct DeliveryRecord {
    std::uint64_t         executor_index = 0;
    agentengine::Message  payload;
};

// ae-naming-lint: allow ExecutorOutputRecord — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ExecutorOutputRecord {
    std::string           executor_id;
    std::uint32_t         round = 0;
    agentengine::Message  payload;
};

// GitHub issue #52 fix, GENERALIZED by issue #62: checkpoint-record twin of
// `WorkflowSupervisor::HeldFanIn` -- see that struct's own comment. Optional on read (default:
// empty), matching this record's existing stall_streak/resets_used precedent for a field added
// after the record shape was first shipped. `awaiting_sources` was `awaiting_recovery` before issue
// #62's fix widened this from "hold only a fallback-recovery rejoin" to "hold every fan_in target
// until every declared source resolves" -- renamed to match what it now tracks; this codebase makes
// no checkpoint-format back-compat promise across a code version change (file banner's own
// single-slot/latest-only narrowing already disclosed that).
// ae-naming-lint: allow HeldFanInRecord — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct HeldFanInRecord {
    std::uint64_t              executor_index = 0;
    agentengine::Message       payload;
    bool                        seeded = false;
    std::vector<std::uint64_t> awaiting_sources;
};

// ae-naming-lint: allow OpenPortRecord — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct OpenPortRecord {
    agentengine::Interaction interaction;
    std::uint64_t             executor_index = 0;
    agentengine::Message      response;
    std::vector<std::string>  routes;
    bool                       resolved = false;
};

// ae-naming-lint: allow RunStateRecord — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct RunStateRecord {
    std::uint64_t  run_counter = 0;
    std::string    run_id;
    std::uint32_t  rounds = 0;
    std::vector<DeliveryRecord>       pending;
    std::vector<ExecutorOutputRecord> partial;
    agentengine::Message  selected_output;
    std::string           failed_executor;
    std::vector<std::string> unopened_ports;
    std::int64_t              elapsed_ns = 0;
    std::vector<OpenPortRecord> ports;
    // ADR-149 (issue #28 item 2/5): must round-trip through checkpoint/resume, or a host that
    // checkpoints a Magentic run at all (the normal, encouraged persistence pattern) would silently
    // reset stall bookkeeping to zero on every resume -- an unlimited-stall-budget bypass of the
    // exact safety valve this field exists to provide. Optional on read (default 0) so a checkpoint
    // taken before ADR-149 still decodes.
    std::uint32_t stall_streak = 0;
    std::uint32_t resets_used  = 0;
    // GitHub issue #52 fix, generalized by issue #62: see HeldFanInRecord's own comment.
    std::vector<HeldFanInRecord> held_fan_in;
};

// interaction_to_json()/interaction_from_json() live in interaction_codec.hpp -- shared with
// rt::AgentSession's own record codec (see that header's own banner for why this used to be a
// duplicated copy here and isn't anymore -- the two copies were byte-identical apart from their
// error code's own dotted suffix, now unified to "rt.interaction.record.malformed").

[[nodiscard]] inline agentengine::json::Value delivery_record_to_json(DeliveryRecord const& d) {
    return agentengine::json::Value::make_object({
        {"executor_index", agentengine::json::Value::make_number(static_cast<double>(d.executor_index))},
        {"payload", message_to_json(d.payload)},
    });
}
[[nodiscard]] inline agentengine::result<DeliveryRecord> delivery_record_from_json(
    agentengine::json::Value const& v) {
    agentengine::json::Value const* executor_index = v.find("executor_index");
    agentengine::json::Value const* payload         = v.find("payload");
    if (executor_index == nullptr || !executor_index->is_number() || payload == nullptr) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "malformed DeliveryRecord",
                                                    "rt.workflow_supervisor.record.malformed"});
    }
    agentengine::result<agentengine::Message> msg = message_from_json(*payload);
    if (!msg) return std::unexpected(msg.error());
    DeliveryRecord d;
    d.executor_index = static_cast<std::uint64_t>(executor_index->as_number());
    d.payload         = std::move(*msg);
    return d;
}

[[nodiscard]] inline agentengine::json::Value executor_output_record_to_json(ExecutorOutputRecord const& o) {
    return agentengine::json::Value::make_object({
        {"executor_id", agentengine::json::Value::make_string(o.executor_id)},
        {"round", agentengine::json::Value::make_number(static_cast<double>(o.round))},
        {"payload", message_to_json(o.payload)},
    });
}
[[nodiscard]] inline agentengine::result<ExecutorOutputRecord> executor_output_record_from_json(
    agentengine::json::Value const& v) {
    agentengine::json::Value const* executor_id = v.find("executor_id");
    agentengine::json::Value const* round        = v.find("round");
    agentengine::json::Value const* payload      = v.find("payload");
    if (executor_id == nullptr || !executor_id->is_string() || round == nullptr ||
        !round->is_number() || payload == nullptr) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "malformed ExecutorOutputRecord",
                                                    "rt.workflow_supervisor.record.malformed"});
    }
    agentengine::result<agentengine::Message> msg = message_from_json(*payload);
    if (!msg) return std::unexpected(msg.error());
    ExecutorOutputRecord o;
    o.executor_id = executor_id->as_string();
    o.round       = static_cast<std::uint32_t>(round->as_number());
    o.payload     = std::move(*msg);
    return o;
}

[[nodiscard]] inline agentengine::json::Value held_fan_in_record_to_json(HeldFanInRecord const& h) {
    std::vector<agentengine::json::Value> awaiting;
    awaiting.reserve(h.awaiting_sources.size());
    for (std::uint64_t const idx : h.awaiting_sources) {
        awaiting.push_back(agentengine::json::Value::make_number(static_cast<double>(idx)));
    }
    return agentengine::json::Value::make_object({
        {"executor_index", agentengine::json::Value::make_number(static_cast<double>(h.executor_index))},
        {"payload", message_to_json(h.payload)},
        {"seeded", agentengine::json::Value::make_bool(h.seeded)},
        {"awaiting_sources", agentengine::json::Value::make_array(std::move(awaiting))},
    });
}
[[nodiscard]] inline agentengine::result<HeldFanInRecord> held_fan_in_record_from_json(
    agentengine::json::Value const& v) {
    agentengine::json::Value const* executor_index   = v.find("executor_index");
    agentengine::json::Value const* payload          = v.find("payload");
    agentengine::json::Value const* seeded           = v.find("seeded");
    agentengine::json::Value const* awaiting_sources = v.find("awaiting_sources");
    if (executor_index == nullptr || !executor_index->is_number() || payload == nullptr ||
        seeded == nullptr || !seeded->is_bool() || awaiting_sources == nullptr ||
        !awaiting_sources->is_array()) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "malformed HeldFanInRecord",
                                                    "rt.workflow_supervisor.record.malformed"});
    }
    agentengine::result<agentengine::Message> msg = message_from_json(*payload);
    if (!msg) return std::unexpected(msg.error());
    HeldFanInRecord h;
    h.executor_index = static_cast<std::uint64_t>(executor_index->as_number());
    h.payload         = std::move(*msg);
    h.seeded          = seeded->as_bool();
    h.awaiting_sources.reserve(awaiting_sources->as_array().size());
    for (agentengine::json::Value const& item : awaiting_sources->as_array()) {
        if (!item.is_number()) {
            return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                        "malformed HeldFanInRecord.awaiting_sources entry",
                                                        "rt.workflow_supervisor.record.malformed"});
        }
        h.awaiting_sources.push_back(static_cast<std::uint64_t>(item.as_number()));
    }
    return h;
}

[[nodiscard]] inline agentengine::json::Value open_port_record_to_json(OpenPortRecord const& p) {
    std::vector<agentengine::json::Value> routes;
    routes.reserve(p.routes.size());
    for (std::string const& r : p.routes) routes.push_back(agentengine::json::Value::make_string(r));
    return agentengine::json::Value::make_object({
        {"interaction", interaction_to_json(p.interaction)},
        {"executor_index", agentengine::json::Value::make_number(static_cast<double>(p.executor_index))},
        {"response", message_to_json(p.response)},
        {"routes", agentengine::json::Value::make_array(std::move(routes))},
        {"resolved", agentengine::json::Value::make_bool(p.resolved)},
    });
}
[[nodiscard]] inline agentengine::result<OpenPortRecord> open_port_record_from_json(
    agentengine::json::Value const& v) {
    agentengine::json::Value const* interaction    = v.find("interaction");
    agentengine::json::Value const* executor_index = v.find("executor_index");
    agentengine::json::Value const* response        = v.find("response");
    agentengine::json::Value const* routes          = v.find("routes");
    agentengine::json::Value const* resolved        = v.find("resolved");
    if (interaction == nullptr || executor_index == nullptr || !executor_index->is_number() ||
        response == nullptr || routes == nullptr || !routes->is_array() || resolved == nullptr ||
        !resolved->is_bool()) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "malformed OpenPortRecord",
                                                    "rt.workflow_supervisor.record.malformed"});
    }
    agentengine::result<agentengine::Interaction> ia = interaction_from_json(*interaction);
    if (!ia) return std::unexpected(ia.error());
    agentengine::result<agentengine::Message> resp = message_from_json(*response);
    if (!resp) return std::unexpected(resp.error());
    OpenPortRecord p;
    p.interaction     = std::move(*ia);
    p.executor_index  = static_cast<std::uint64_t>(executor_index->as_number());
    p.response        = std::move(*resp);
    p.routes.reserve(routes->as_array().size());
    for (agentengine::json::Value const& r : routes->as_array()) {
        if (!r.is_string()) {
            return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                        "malformed OpenPortRecord.routes entry",
                                                        "rt.workflow_supervisor.record.malformed"});
        }
        p.routes.push_back(r.as_string());
    }
    p.resolved = resolved->as_bool();
    return p;
}

[[nodiscard]] inline agentengine::json::Value run_state_record_to_json(RunStateRecord const& rec) {
    std::vector<agentengine::json::Value> pending;
    pending.reserve(rec.pending.size());
    for (auto const& d : rec.pending) pending.push_back(delivery_record_to_json(d));

    std::vector<agentengine::json::Value> partial;
    partial.reserve(rec.partial.size());
    for (auto const& o : rec.partial) partial.push_back(executor_output_record_to_json(o));

    std::vector<agentengine::json::Value> unopened_ports;
    unopened_ports.reserve(rec.unopened_ports.size());
    for (std::string const& id : rec.unopened_ports) {
        unopened_ports.push_back(agentengine::json::Value::make_string(id));
    }

    std::vector<agentengine::json::Value> ports;
    ports.reserve(rec.ports.size());
    for (auto const& p : rec.ports) ports.push_back(open_port_record_to_json(p));

    std::vector<agentengine::json::Value> held_fan_in;
    held_fan_in.reserve(rec.held_fan_in.size());
    for (auto const& h : rec.held_fan_in) held_fan_in.push_back(held_fan_in_record_to_json(h));

    return agentengine::json::Value::make_object({
        {"run_counter", agentengine::json::Value::make_number(static_cast<double>(rec.run_counter))},
        {"run_id", agentengine::json::Value::make_string(rec.run_id)},
        {"rounds", agentengine::json::Value::make_number(static_cast<double>(rec.rounds))},
        {"pending", agentengine::json::Value::make_array(std::move(pending))},
        {"partial", agentengine::json::Value::make_array(std::move(partial))},
        {"selected_output", message_to_json(rec.selected_output)},
        {"failed_executor", agentengine::json::Value::make_string(rec.failed_executor)},
        {"unopened_ports", agentengine::json::Value::make_array(std::move(unopened_ports))},
        {"elapsed_ns", agentengine::json::Value::make_number(static_cast<double>(rec.elapsed_ns))},
        {"ports", agentengine::json::Value::make_array(std::move(ports))},
        {"stall_streak", agentengine::json::Value::make_number(static_cast<double>(rec.stall_streak))},
        {"resets_used", agentengine::json::Value::make_number(static_cast<double>(rec.resets_used))},
        {"held_fan_in", agentengine::json::Value::make_array(std::move(held_fan_in))},
    });
}

[[nodiscard]] inline agentengine::result<RunStateRecord> run_state_record_from_json(
    agentengine::json::Value const& v) {
    agentengine::json::Value const* run_counter      = v.find("run_counter");
    agentengine::json::Value const* run_id           = v.find("run_id");
    agentengine::json::Value const* rounds           = v.find("rounds");
    agentengine::json::Value const* pending          = v.find("pending");
    agentengine::json::Value const* partial          = v.find("partial");
    agentengine::json::Value const* selected_output  = v.find("selected_output");
    agentengine::json::Value const* failed_executor  = v.find("failed_executor");
    agentengine::json::Value const* unopened_ports   = v.find("unopened_ports");
    agentengine::json::Value const* elapsed_ns       = v.find("elapsed_ns");
    agentengine::json::Value const* ports            = v.find("ports");
    if (run_counter == nullptr || !run_counter->is_number() || run_id == nullptr ||
        !run_id->is_string() || rounds == nullptr || !rounds->is_number() || pending == nullptr ||
        !pending->is_array() || partial == nullptr || !partial->is_array() ||
        selected_output == nullptr || failed_executor == nullptr || !failed_executor->is_string() ||
        unopened_ports == nullptr || !unopened_ports->is_array() || elapsed_ns == nullptr ||
        !elapsed_ns->is_number() || ports == nullptr || !ports->is_array()) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "malformed RunStateRecord",
                                                    "rt.workflow_supervisor.record.malformed"});
    }
    RunStateRecord rec;
    rec.run_counter = static_cast<std::uint64_t>(run_counter->as_number());
    rec.run_id      = run_id->as_string();
    rec.rounds      = static_cast<std::uint32_t>(rounds->as_number());
    rec.pending.reserve(pending->as_array().size());
    for (agentengine::json::Value const& item : pending->as_array()) {
        auto d = delivery_record_from_json(item);
        if (!d) return std::unexpected(d.error());
        rec.pending.push_back(std::move(*d));
    }
    rec.partial.reserve(partial->as_array().size());
    for (agentengine::json::Value const& item : partial->as_array()) {
        auto o = executor_output_record_from_json(item);
        if (!o) return std::unexpected(o.error());
        rec.partial.push_back(std::move(*o));
    }
    agentengine::result<agentengine::Message> sel = message_from_json(*selected_output);
    if (!sel) return std::unexpected(sel.error());
    rec.selected_output = std::move(*sel);
    rec.failed_executor  = failed_executor->as_string();
    rec.unopened_ports.reserve(unopened_ports->as_array().size());
    for (agentengine::json::Value const& item : unopened_ports->as_array()) {
        if (!item.is_string()) {
            return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                        "malformed RunStateRecord.unopened_ports entry",
                                                        "rt.workflow_supervisor.record.malformed"});
        }
        rec.unopened_ports.push_back(item.as_string());
    }
    rec.elapsed_ns = static_cast<std::int64_t>(elapsed_ns->as_number());
    rec.ports.reserve(ports->as_array().size());
    for (agentengine::json::Value const& item : ports->as_array()) {
        auto p = open_port_record_from_json(item);
        if (!p) return std::unexpected(p.error());
        rec.ports.push_back(std::move(*p));
    }
    // ADR-149: optional on read -- a pre-ADR-149 checkpoint has neither field, and 0 is the correct
    // "no stall bookkeeping yet" value for one, matching this record's other additive-field precedent.
    if (agentengine::json::Value const* stall_streak = v.find("stall_streak");
        stall_streak != nullptr && stall_streak->is_number()) {
        rec.stall_streak = static_cast<std::uint32_t>(stall_streak->as_number());
    }
    if (agentengine::json::Value const* resets_used = v.find("resets_used");
        resets_used != nullptr && resets_used->is_number()) {
        rec.resets_used = static_cast<std::uint32_t>(resets_used->as_number());
    }
    // GitHub issue #52 fix: optional on read -- a pre-fix checkpoint has no such field, and "no
    // outstanding holds" is the correct value for one, matching stall_streak/resets_used's own
    // precedent immediately above.
    if (agentengine::json::Value const* held_fan_in = v.find("held_fan_in");
        held_fan_in != nullptr && held_fan_in->is_array()) {
        rec.held_fan_in.reserve(held_fan_in->as_array().size());
        for (agentengine::json::Value const& item : held_fan_in->as_array()) {
            auto h = held_fan_in_record_from_json(item);
            if (!h) return std::unexpected(h.error());
            rec.held_fan_in.push_back(std::move(*h));
        }
    }
    return rec;
}

[[nodiscard]] inline std::vector<std::byte> encode_run_state_record(RunStateRecord const& rec) {
    std::string const text = agentengine::json::dump(run_state_record_to_json(rec));
    auto const* const first = reinterpret_cast<std::byte const*>(text.data());
    return std::vector<std::byte>(first, first + text.size());
}
[[nodiscard]] inline agentengine::result<RunStateRecord> decode_run_state_record(
    std::vector<std::byte> const& bytes) {
    std::string const text(reinterpret_cast<char const*>(bytes.data()), bytes.size());
    agentengine::result<agentengine::json::Value> parsed = agentengine::json::parse(text);
    if (!parsed) return std::unexpected(parsed.error());
    return run_state_record_from_json(*parsed);
}

}  // namespace agentengine::rt
