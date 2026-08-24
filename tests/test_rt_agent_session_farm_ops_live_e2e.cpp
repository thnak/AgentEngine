// Implements 009-Plugin-and-Extension-System.md §8 (multi-skill), decisions/ADR-065-tool-optimizer-
// provider.md (a REAL ToolOptimizerProvider, not a stand-in), and 010-Python-Code-Interpreter.md §1a/
// §2 (a REAL embedded CPython interpreter, native_jail::PythonRunner -- not a canned stand-in), all
// driven through a real remote model over OpenRouter.
//
// This is a deliberate expansion of test_rt_agent_session_farm_advisory_live_e2e.cpp's single-skill/
// single-tool scenario into something closer to what tools/cli_chat.cpp actually offers: THREE
// independent skills covering three different farm decisions (crop irrigation/spraying, livestock
// care, harvest planning), so a farmer running a mixed operation can reach any of them by describing
// what they need, not by naming a tool; a SEPARATE, larger scheduling/market tool pool gated through a
// real ToolOptimizerProvider
// (ADR-065's search_tools/mount_tool/unmount_tool) rather than always-on declaration, so that
// mechanism has an actual reason to matter (five tools is few enough to read but enough that
// "just declare them all" and "search, then mount only what's needed" are visibly different
// strategies); and one genuinely real tool, execute_code, backed by the real embedded CPython
// interpreter used elsewhere in this codebase (native_jail::PythonRunner, sandbox/runner.hpp's Runner
// concept) instead of a deterministic arithmetic stand-in -- proving actual sandboxed computation, not
// just tool selection.
//
// Skill-gated tools (crop-field-operations -> check_field_weather/check_pest_pressure;
// livestock-care-operations -> check_animal_health/calculate_feed_ration) stay canned stand-ins, same
// convention as the two tests above ("a deterministic stand-in, not a real weather API") -- the point
// of THOSE tools is proving skill-driven tool SELECTION, not computation. execute_code is the one tool
// in this file that is not a stand-in: its result comes from running real Python source through a real
// interpreter, checked structurally (the exact expected numeric answer appears in the real output),
// which a canned reply could not produce by construction.
//
// Composition: this file's own FarmOpsProvider owns its own SkillsProvider<> (two skills), its own
// MountedSkillsState (skill mount gating, same shape as the file above), AND its own real
// ToolOptimizerProvider instance (composed via plain member delegation, not another ContextProvider
// layer -- ToolOptimizerProvider::on_context()'s ContextContribution carries only `.tools`, never
// `.messages`, so folding its output into FarmOpsProvider's own contribution alongside the skill
// system text and history is exactly the "compose raw ToolDescriptor sources directly" idiom
// tool_optimizer_provider.hpp's own top comment describes -- never a second on_context() hop through
// AgentSession itself, since AgentSession has exactly one HistoryProviderT slot (I1)).
//
// CREDENTIALS ARE NEVER COMPILED IN (018 §4) -- same environment-variable contract as every other live
// test in this suite: AGENTENGINE_OPENROUTER_API_KEY (required, else SKIP), AGENTENGINE_OPENROUTER_MODEL
// (optional), AGENTENGINE_OPENROUTER_HOST (optional). tools/run-live-provider-tests.ps1 populates these
// from a local, git-ignored key file; nothing here reads a file directly.
//
// Labelled `live-network` (tests/CMakeLists.txt) -- excluded from a default `ctest` run, SKIPPED (exit
// 0), not failed, when the API key is absent, and only registered at all when
// AGENTENGINE_BUILD_PYTHON_RUNNER is also ON (needs a real CPython install to link against).

#ifdef AGENTENGINE_WITH_HTTPS

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/mounted_skills_state.hpp"
#include "agentengine/core/skill_provider.hpp"
#include "agentengine/core/skill_source.hpp"
#include "agentengine/core/skill_tool_scoping.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/core/tool_optimizer_provider.hpp"
#include "agentengine/pal/env.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/sandbox/runner.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"
#include "backends/native_jail/python_runner.hpp"

using namespace agentengine;
using agentengine::rt::AgentResponse;
using agentengine::rt::AgentSession;
using agentengine::rt::NoSessionState;
using agentengine::rt::StartRun;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}
void note(char const* label, std::string const& value) {
    std::fprintf(stderr, "  .. %s = %s\n", label, value.c_str());
}

[[nodiscard]] std::string env_or(char const* name, std::string fallback) {
    auto const v = ::agentengine::pal::env_var(name);
    return (v && !v->empty()) ? *v : std::move(fallback);
}

// Same "safe here because nothing genuinely suspends externally" drive<T>() every other
// test_rt_agent_session*.cpp file uses.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

constexpr char const* kDefaultModel = "stealth/ox-alpha";
constexpr char const* kDefaultHost = "openrouter.ai";
constexpr std::uint16_t kHttpsPort = 443;
constexpr char const* kPathPrefix = "/api/v1";
constexpr char const* kSecretName = "openrouter-api-key";
// OpenRouter's dashboard Activity view groups/labels rows by this (the `X-Title` header), NOT by the
// `user` field (`end_user_id` below) -- confirmed directly against a real run: without a per-file
// X-Title, every case here reads as an anonymous, unlabeled request no matter how distinct its
// `end_user_id` is. This is the SEPARATE field that makes this file's own traffic findable in the
// dashboard at all -- `end_user_id` does NOT drive OpenRouter's own cache routing (docs/research/
// 2026-08-21-openrouter-session-id-header.md corrects an earlier claim here that it did); the new
// `session_id` constructor param below is what actually keys OpenRouter's sticky routing.
constexpr char const* kXTitle = "AgentEngine: farm-ops-live-e2e";
constexpr char const* kCropSkillName = "crop-field-operations";
constexpr char const* kLivestockSkillName = "livestock-care-operations";
constexpr char const* kHarvestSkillName = "harvest-planning-operations";

// ================= Call logs -- AgentSession::start_run() resolves the whole tool loop internally
// (ADR-027), so what actually got invoked, and with what argument, is observed from inside the tool
// bodies themselves, the same function-local-static idiom every other live test in this suite uses. ==
[[nodiscard]] std::vector<std::string>& mounted_skill_names_log() {
    static std::vector<std::string> names;
    return names;
}
[[nodiscard]] bool& check_weather_called_log() {
    static bool called = false;
    return called;
}
[[nodiscard]] bool& check_pest_called_log() {
    static bool called = false;
    return called;
}
[[nodiscard]] bool& check_animal_health_called_log() {
    static bool called = false;
    return called;
}
[[nodiscard]] bool& calculate_feed_ration_called_log() {
    static bool called = false;
    return called;
}
[[nodiscard]] bool& check_harvest_weather_called_log() {
    static bool called = false;
    return called;
}
[[nodiscard]] bool& estimate_yield_called_log() {
    static bool called = false;
    return called;
}
[[nodiscard]] bool& execute_code_called_log() {
    static bool called = false;
    return called;
}
[[nodiscard]] std::string& observed_code_arg_log() {
    static std::string code;
    return code;
}
[[nodiscard]] std::string& observed_code_output_log() {
    static std::string output;
    return output;
}
[[nodiscard]] bool& get_market_price_called_log() {
    static bool called = false;
    return called;
}
[[nodiscard]] std::string& observed_commodity_log() {
    static std::string commodity;
    return commodity;
}

// ================= Two skills, one source, mirroring builtin_skills.hpp's own multi-skill pattern
// (one make_*_source() eagerly parsing several SKILL.md literals into one std::vector<SkillSourceResult>).

// Live evidence (dumps/dump-5.json and a cross-model check against openai/gpt-5.6-luna, both
// reproducing the identical pattern): naming two required tools was not enough -- both models
// consistently called only the tool that most directly produced the user's asked-for ANSWER (pest
// pressure -> spray decision; feed ration -> the number asked for) and skipped the more
// confirmatory tool, even though the skill said to call both. The wording below is deliberately
// more forceful ("MUST call BOTH... incomplete and wrong") and explicitly names why neither tool
// substitutes for the other, rather than just listing both names once.
inline constexpr std::string_view kCropSkillMd = R"SKILL(---
name: crop-field-operations
description: Decide irrigation and pesticide-spraying actions for a crop field, using real weather and pest-pressure data for that field. Use this for a farmer growing crops (rice, vegetables, and similar) asking about a named field.
allowed-tools: check_field_weather, check_pest_pressure
metadata:
  version: "1"
---
# Crop field operations

Before recommending irrigation or pesticide spraying for a named field, you MUST call BOTH
`check_field_weather` AND `check_pest_pressure` for that field's name. An answer based on only one
of the two is incomplete and wrong -- do not skip either call, and do not treat one reading as a
substitute for the other: pest pressure alone does not tell you the forecast, and the forecast
alone does not tell you pest pressure. Do not guess either value.

## Reading the results

- If the forecast shows no rain and pest pressure is moderate or high: spraying is warranted, and
  irrigation may proceed if the field needs it.
- If rain is expected: postpone both -- irrigation is redundant and spraying would wash off.
- If pest pressure is low: hold off on pesticide regardless of the forecast.

Always state both the forecast and the pest-pressure reading you actually received before giving
your recommendation.
)SKILL";

inline constexpr std::string_view kLivestockSkillMd = R"SKILL(---
name: livestock-care-operations
description: Decide feeding and health-check actions for livestock (cattle, poultry, and similar), using real herd data for a named herd or flock. Use this for a farmer raising animals asking about care for a named group.
allowed-tools: check_animal_health, calculate_feed_ration
metadata:
  version: "1"
---
# Livestock care operations

Before recommending a feeding plan for a named herd or flock, you MUST call BOTH
`check_animal_health` AND `calculate_feed_ration` (with that group's name, its animal count, and
its animal type). An answer based on only one of the two is incomplete and wrong -- do not skip
either call, and do not treat one as a substitute for the other: the ration number alone does not
tell you whether the flock is healthy, and a health status alone does not tell you how much to
feed. Do not guess either value.

## Reading the results

- If health status is anything other than healthy: recommend addressing the health issue before
  changing the feeding plan.
- Otherwise: report the computed daily ration and confirm it matches the group's normal schedule.
)SKILL";

// THIRD independent farm model: harvest planning, covering yet another decision (timing + yield)
// that structurally needs two tools together, same as the two skills above -- widens the scenario
// per the project owner's own request to bring this test closer to a real multi-domain farm
// operation, and gives a third, independent data point on the "call both tools" wording fix.
inline constexpr std::string_view kHarvestSkillMd = R"SKILL(---
name: harvest-planning-operations
description: Decide whether to harvest a named field this week and what yield to expect, using real weather-window and yield-estimate data for that field. Use this for a farmer growing crops asking about harvest timing or expected yield for a named field.
allowed-tools: check_harvest_weather_window, estimate_harvest_yield
metadata:
  version: "1"
---
# Harvest planning operations

Before recommending a harvest timing or reporting an expected yield for a named field, you MUST
call BOTH `check_harvest_weather_window` AND `estimate_harvest_yield` for that field. An answer
based on only one of the two is incomplete and wrong: the weather window tells you WHEN it is safe
to harvest, and the yield estimate tells you HOW MUCH to expect -- neither substitutes for the
other. Do not guess either value.

## Reading the results

- If the weather window is not dry enough: recommend waiting, regardless of the yield estimate.
- If the weather window is dry enough: recommend harvesting now, and report the estimated yield.
)SKILL";

[[nodiscard]] SkillSourceDescriptor make_farm_ops_skills_source() {
    result<std::vector<SkillSourceResult>> resolved = [] {
        std::vector<SkillSourceResult> out;
        struct Entry { std::string_view md; char const* name; };
        Entry const entries[] = {
            {kCropSkillMd, "crop-field-operations"},
            {kLivestockSkillMd, "livestock-care-operations"},
            {kHarvestSkillMd, "harvest-planning-operations"},
        };
        for (auto const& e : entries) {
            auto skill = parse_skill_md(e.md, e.name);
            if (!skill) return result<std::vector<SkillSourceResult>>(std::unexpected(skill.error()));
            std::string const text(e.md);
            std::vector<std::byte> bytes(reinterpret_cast<std::byte const*>(text.data()),
                                          reinterpret_cast<std::byte const*>(text.data()) + text.size());
            std::vector<SkillBundleFile> files;
            files.push_back(SkillBundleFile{"SKILL.md", std::move(bytes)});
            out.push_back(SkillSourceResult{std::move(*skill), std::move(files)});
        }
        return result<std::vector<SkillSourceResult>>(std::move(out));
    }();

    SkillSourceDescriptor d;
    d.origin_id = "farm-ops-test";
    d.load_skills = [resolved]() { return resolved; };
    return d;
}

// ================= Skill-gated tools (canned stand-ins -- see this file's own top comment) =========

struct FieldQueryArgs {
    std::string field_name;
};
AE_JSON_SCHEMA(FieldQueryArgs, field_name)

struct FieldWeatherReply {
    std::string forecast;
    std::string advice;
};
AE_JSON_SCHEMA(FieldWeatherReply, forecast, advice)

struct CheckFieldWeatherTool
    : Tool<CheckFieldWeatherTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "check_field_weather";
    static constexpr std::string_view description =
        "Gets today's local weather forecast for a named field, for deciding whether to irrigate or "
        "spray it.";
    using Args = FieldQueryArgs;
    using Reply = FieldWeatherReply;
    static result<Reply> invoke(Args a, EffectContext&) {
        check_weather_called_log() = true;
        return Reply{"Clear skies, no rain expected for 3 days, around 34C.",
                     "No rain in the forecast for " + a.field_name +
                         " -- irrigation may proceed if the field needs it. Check pest pressure "
                         "before deciding on spraying."};
    }
};

struct PestPressureReply {
    std::string level;
    std::string recommendation;
};
AE_JSON_SCHEMA(PestPressureReply, level, recommendation)

struct CheckPestPressureTool
    : Tool<CheckPestPressureTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "check_pest_pressure";
    static constexpr std::string_view description =
        "Gets today's measured pest pressure for a named field, for deciding whether spraying is "
        "warranted.";
    using Args = FieldQueryArgs;
    using Reply = PestPressureReply;
    static result<Reply> invoke(Args a, EffectContext&) {
        check_pest_called_log() = true;
        // Deliberately says NOTHING about weather/forecast -- an earlier version mentioned "the dry
        // forecast" here, and a captured live transcript showed the model treating that as if it had
        // already learned the forecast, never calling check_field_weather at all, then looping on
        // repeated redundant check_pest_pressure calls instead. This tool answers ONE question
        // (pest pressure) and nothing else, so there is no shortcut around calling the other tool.
        return Reply{"high", "Aphid pressure on " + a.field_name +
                                  " is high, which alone warrants pesticide treatment. This says "
                                  "nothing about the weather -- check that separately."};
    }
};

struct HerdQueryArgs {
    std::string herd_name;
};
AE_JSON_SCHEMA(HerdQueryArgs, herd_name)

struct AnimalHealthReply {
    std::string status;
    std::string notes;
};
AE_JSON_SCHEMA(AnimalHealthReply, status, notes)

struct CheckAnimalHealthTool
    : Tool<CheckAnimalHealthTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "check_animal_health";
    static constexpr std::string_view description =
        "Gets today's health status for a named herd or flock, for deciding on its feeding plan.";
    using Args = HerdQueryArgs;
    using Reply = AnimalHealthReply;
    static result<Reply> invoke(Args a, EffectContext&) {
        check_animal_health_called_log() = true;
        return Reply{"healthy",
                     "No signs of illness in " + a.herd_name + " -- proceed with standard feeding."};
    }
};

struct FeedRationArgs {
    std::string herd_name;
    int animal_count = 0;
    std::string animal_type;
};
AE_JSON_SCHEMA(FeedRationArgs, herd_name, animal_count, animal_type)

struct FeedRationReply {
    double ration_kg_per_day = 0.0;
    std::string notes;
};
AE_JSON_SCHEMA(FeedRationReply, ration_kg_per_day, notes)

// Deterministic per-animal-type feed rate -- a plausible stand-in table, not a real nutrition model
// (this file's own top comment: the skill-gated tools stay canned; execute_code is the one real one).
[[nodiscard]] double feed_rate_kg_per_animal(std::string const& animal_type) {
    if (animal_type == "chicken" || animal_type == "poultry") return 0.12;
    if (animal_type == "cattle" || animal_type == "cow") return 11.0;
    if (animal_type == "pig") return 2.3;
    return 1.0;
}

struct CalculateFeedRationTool
    : Tool<CalculateFeedRationTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "calculate_feed_ration";
    static constexpr std::string_view description =
        "Computes today's total feed ration (kg) for a named herd or flock, given its animal count "
        "and animal type.";
    using Args = FeedRationArgs;
    using Reply = FeedRationReply;
    static result<Reply> invoke(Args a, EffectContext&) {
        calculate_feed_ration_called_log() = true;
        double const total = static_cast<double>(a.animal_count) * feed_rate_kg_per_animal(a.animal_type);
        return Reply{total, a.herd_name + "'s ration is based on " + std::to_string(a.animal_count) +
                                 " " + a.animal_type + "(s) at the standard per-animal rate."};
    }
};

// harvest-planning-operations' two tools -- same canned-stand-in convention as the crop/livestock
// pairs above.
struct HarvestFieldArgs {
    std::string field_name;
};
AE_JSON_SCHEMA(HarvestFieldArgs, field_name)

struct HarvestWeatherWindowReply {
    bool dry_enough = false;
    std::string advice;
};
AE_JSON_SCHEMA(HarvestWeatherWindowReply, dry_enough, advice)

struct CheckHarvestWeatherWindowTool
    : Tool<CheckHarvestWeatherWindowTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "check_harvest_weather_window";
    static constexpr std::string_view description =
        "Checks whether the coming days are dry enough to safely harvest a named field.";
    using Args = HarvestFieldArgs;
    using Reply = HarvestWeatherWindowReply;
    static result<Reply> invoke(Args a, EffectContext&) {
        check_harvest_weather_called_log() = true;
        return Reply{true, "The next 5 days for " + a.field_name + " are dry -- safe to harvest now."};
    }
};

struct YieldEstimateArgs {
    std::string field_name;
    double hectares = 0.0;
    std::string crop_type;
};
AE_JSON_SCHEMA(YieldEstimateArgs, field_name, hectares, crop_type)

struct YieldEstimateReply {
    double estimated_yield_kg = 0.0;
    std::string notes;
};
AE_JSON_SCHEMA(YieldEstimateReply, estimated_yield_kg, notes)

[[nodiscard]] double yield_rate_kg_per_hectare(std::string const& crop_type) {
    if (crop_type == "rice") return 4500.0;
    if (crop_type == "vegetables" || crop_type == "vegetable") return 12000.0;
    return 3000.0;
}

struct EstimateHarvestYieldTool
    : Tool<EstimateHarvestYieldTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "estimate_harvest_yield";
    static constexpr std::string_view description =
        "Estimates the harvest yield (kg) for a named field, given its area (hectares) and crop type.";
    using Args = YieldEstimateArgs;
    using Reply = YieldEstimateReply;
    static result<Reply> invoke(Args a, EffectContext&) {
        estimate_yield_called_log() = true;
        double const total = a.hectares * yield_rate_kg_per_hectare(a.crop_type);
        return Reply{total, a.field_name + "'s estimated yield is based on " + std::to_string(a.hectares) +
                                 " hectares of " + a.crop_type + " at the standard per-hectare rate."};
    }
};

// mount_skill -- identical role/shape to the two files above (ADR-024 Phase 3 addendum), gating ALL
// THREE skills through the same one tool: which skill gets mounted depends only on the `skill_name`
// argument.
struct MountSkillArgs {
    std::string skill_name;
};
AE_JSON_SCHEMA(MountSkillArgs, skill_name)

struct MountSkillReply {
    bool ok = false;
    std::string message;
};
AE_JSON_SCHEMA(MountSkillReply, ok, message)

struct MountSkillTool : Tool<MountSkillTool, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "mount_skill";
    static constexpr std::string_view description =
        "Activate a skill you've seen advertised by name and description, so its full instructions "
        "become part of your context and any tools it names become callable -- starting next turn. "
        "This does not read or return the skill's content; call it before you need a skill's own "
        "tools, then wait for your next turn to use them.";
    using Args = MountSkillArgs;
    using Reply = MountSkillReply;
    // ADR-030: unreachable poison -- real logic lives in FarmOpsProvider::real_mount_skill().
    static result<Reply> invoke(Args, EffectContext&) {
        return std::unexpected(error{failure_class::fatal,
                                      "MountSkillTool::invoke() must never run directly -- reached "
                                      "only through FarmOpsProvider::real_mount_skill()",
                                      "farm_ops.dead_static_invoke_path"});
    }
};

// ================= ToolOptimizerProvider (ADR-065) pool -- scheduling/market tools, NEVER always-on:
// the model must search_tools/mount_tool before any of these are even declared. Deliberately five
// tools (not one or two) so "search, then mount only what's needed" is a real strategy difference from
// "declare everything," which is the whole point of exercising this mechanism here. ====================

struct ScheduleTaskArgs {
    std::string description;
    int due_in_days = 0;
};
AE_JSON_SCHEMA(ScheduleTaskArgs, description, due_in_days)

struct ScheduleTaskReply {
    bool ok = false;
    std::string message;
};
AE_JSON_SCHEMA(ScheduleTaskReply, ok, message)

[[nodiscard]] std::vector<std::string>& scheduled_tasks_log() {
    static std::vector<std::string> tasks;
    return tasks;
}

struct ScheduleTaskTool : Tool<ScheduleTaskTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "schedule_task";
    static constexpr std::string_view description =
        "Schedules a farm task (irrigation run, vet visit, harvest, market trip, etc.) to happen in "
        "a given number of days from today.";
    using Args = ScheduleTaskArgs;
    using Reply = ScheduleTaskReply;
    static result<Reply> invoke(Args a, EffectContext&) {
        scheduled_tasks_log().push_back(a.description + " (in " + std::to_string(a.due_in_days) +
                                         " day(s))");
        return Reply{true, "scheduled: " + a.description};
    }
};

struct CancelTaskArgs {
    std::string description;
};
AE_JSON_SCHEMA(CancelTaskArgs, description)

struct CancelTaskReply {
    bool ok = false;
    std::string message;
};
AE_JSON_SCHEMA(CancelTaskReply, ok, message)

struct CancelTaskTool : Tool<CancelTaskTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "cancel_task";
    static constexpr std::string_view description =
        "Cancels a previously scheduled farm task by its description text.";
    using Args = CancelTaskArgs;
    using Reply = CancelTaskReply;
    static result<Reply> invoke(Args a, EffectContext&) {
        auto& tasks = scheduled_tasks_log();
        auto it = std::find_if(tasks.begin(), tasks.end(), [&](std::string const& t) {
            return t.find(a.description) != std::string::npos;
        });
        if (it == tasks.end()) {
            return Reply{false, "no scheduled task matched: " + a.description};
        }
        tasks.erase(it);
        return Reply{true, "cancelled: " + a.description};
    }
};

struct MarketPriceArgs {
    std::string commodity;
};
AE_JSON_SCHEMA(MarketPriceArgs, commodity)

struct MarketPriceReply {
    std::string commodity;
    double price_per_kg = 0.0;
};
AE_JSON_SCHEMA(MarketPriceReply, commodity, price_per_kg)

[[nodiscard]] double price_per_kg_for(std::string const& commodity) {
    if (commodity == "rice") return 0.42;
    if (commodity == "chicken" || commodity == "poultry") return 3.10;
    if (commodity == "pork" || commodity == "pig") return 2.75;
    return 1.00;
}

struct GetMarketPriceTool
    : Tool<GetMarketPriceTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "get_market_price";
    static constexpr std::string_view description =
        "Gets today's local market price per kilogram for a named commodity (e.g. rice, chicken, "
        "pork), for planning a sale.";
    using Args = MarketPriceArgs;
    using Reply = MarketPriceReply;
    static result<Reply> invoke(Args a, EffectContext&) {
        get_market_price_called_log() = true;
        observed_commodity_log() = a.commodity;
        return Reply{a.commodity, price_per_kg_for(a.commodity)};
    }
};

struct SellOrderArgs {
    std::string commodity;
    double quantity_kg = 0.0;
};
AE_JSON_SCHEMA(SellOrderArgs, commodity, quantity_kg)

struct SellOrderReply {
    bool ok = false;
    std::string message;
    double total_value = 0.0;
};
AE_JSON_SCHEMA(SellOrderReply, ok, message, total_value)

struct PlaceSellOrderTool
    : Tool<PlaceSellOrderTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "place_sell_order";
    static constexpr std::string_view description =
        "Places a sell order for a quantity (kg) of a named commodity at today's market price.";
    using Args = SellOrderArgs;
    using Reply = SellOrderReply;
    static result<Reply> invoke(Args a, EffectContext&) {
        double const total = a.quantity_kg * price_per_kg_for(a.commodity);
        return Reply{true, "sell order placed: " + std::to_string(a.quantity_kg) + "kg of " + a.commodity,
                     total};
    }
};

// execute_code -- the one REAL tool in this file. Backed by native_jail::PythonRunner (sandbox/
// runner.hpp's Runner concept), the same real embedded-CPython backend
// tests/test_python_embed_smoke.cpp proves initializes and runs for real; NOT the mediated/agent.tools
// variant tools/cli_chat.cpp uses (no tool bridge, no worktree mounts, no agent.ask -- this scenario
// has no need for any of that, matching test_rt_agent_session_farm_advisory_live_e2e.cpp's own stated
// reason for leaving that machinery out).
struct ExecuteCodeArgs {
    std::string code;
};
AE_JSON_SCHEMA(ExecuteCodeArgs, code)

struct ExecuteCodeReply {
    bool ok = false;
    std::string stdout_text;
    std::string stderr_text;
    std::string result_repr;
};
AE_JSON_SCHEMA(ExecuteCodeReply, ok, stdout_text, stderr_text, result_repr)

struct ExecuteCodeTool : Tool<ExecuteCodeTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "execute_code";
    static constexpr std::string_view description =
        "Runs a snippet of real Python code in a sandboxed interpreter and returns what it printed "
        "(the `math` module is available). Use this for any calculation you need to get exactly "
        "right -- irrigation volumes, feed totals, sale values -- rather than computing it yourself.";
    using Args = ExecuteCodeArgs;
    using Reply = ExecuteCodeReply;
    // ADR-030: unreachable poison -- real logic lives in FarmOpsProvider::real_execute_code(), which
    // reaches the real, shared, lazily-initialized PythonRunner below.
    static result<Reply> invoke(Args, EffectContext&) {
        return std::unexpected(error{failure_class::fatal,
                                      "ExecuteCodeTool::invoke() must never run directly -- reached "
                                      "only through FarmOpsProvider::real_execute_code()",
                                      "farm_ops.dead_static_invoke_path"});
    }
};

// Process-lifetime lazy singleton, exactly mirroring test_python_embed_smoke.cpp's own construction
// (PythonLockdownConfig{python_home, allowed_top_level_modules} -> PythonRunner -> initialize()) but
// wrapped so it runs exactly once no matter how many times execute_code is actually called. No
// session-claim machinery (tools/cli_chat.cpp's CodeActRunnerBinding) -- this test drives exactly one
// AgentSession synchronously on one thread, so there is no concurrent-session claim to arbitrate (see
// ADR-002 §5.5.6: at most one interpreter alive per process, trivially satisfied here).
[[nodiscard]] PythonRunner& shared_farm_ops_python_runner() {
    static PythonRunner runner = [] {
        native_jail::PythonLockdownConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;
        cfg.allowed_top_level_modules = {"math"};
        return PythonRunner(std::move(cfg));
    }();
    static bool const initialized = runner.initialize();
    (void)initialized;
    return runner;
}

// ================= The ContextProvider: owns its own SkillsProvider<> (two skills) + MountedSkillsState
// (skill-mount gating) AND its own real ToolOptimizerProvider (the scheduling/market/execute_code
// pool's mount/search gating) -- a complete conformer by itself, occupying AgentSession's single
// HistoryProviderT slot directly (mirrors the two files above's FarmAdvisoryProvider/
// ToolDeclaringHistoryProvider for exactly the same reason: this provider already owns the whole
// tool-and-skill-related system prompt). ============================================================
class FarmOpsProvider {
public:
    // decisions/ADR-066-context-provider-attribution-provenance.md §3.
    static constexpr std::string_view name = "farm-ops-live-test";

    FarmOpsProvider()
        : skills_({make_farm_ops_skills_source()}),
          optimizer_(build_optimizer_agent_tools(), no_tool_source(), no_tool_source(), /*always_on=*/{}) {}

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& session_ctx,
                                                                 EffectContext& ec) {
        auto skills_contribution = co_await skills_.on_context(session_ctx, ec);
        if (!skills_contribution) co_return std::unexpected(skills_contribution.error());

        auto optimizer_contribution = co_await optimizer_.on_context(session_ctx, ec);
        if (!optimizer_contribution) co_return std::unexpected(optimizer_contribution.error());

        std::string combined_system_text;
        if (!skills_contribution->messages.empty() && !skills_contribution->messages[0].content.empty()) {
            if (auto const* t = std::get_if<Text>(&skills_contribution->messages[0].content[0].value)) {
                combined_system_text = t->text;
            }
        }
        for (auto const& mounted_name : mounted_skills_.all()) {
            auto body = skills_.body_of(mounted_name);
            if (!body) continue;
            combined_system_text += "\nMounted skill '" + mounted_name + "':\n" + *body;
        }
        // Live-model evidence (captured request/response traces) showed the model repeatedly calling
        // search_tools trying to locate a skill-gated tool by name, getting back an empty result
        // (correct -- search_tools only ever searches ToolOptimizerProvider's OWN pool, never the
        // skill-unlocked tools), and then WRONGLY concluding the tool doesn't exist at all -- even
        // though it was sitting directly in the model's own declared `tools` array the entire time.
        // Stating the two-mechanism POLICY in the abstract (an earlier version of this text) was not
        // enough to stop that: the model apparently does not reliably cross-reference "is this name
        // already in my own tools list" before deciding to search. Naming the currently-callable
        // tools CONCRETELY, by name, removes the inference step entirely.
        std::vector<std::string> const currently_unlocked =
            skills_.allowed_tool_names_for(mounted_skills_.all());
        std::string unlocked_list = std::string(MountSkillTool::name);
        for (auto const& n : currently_unlocked) unlocked_list += ", " + n;
        combined_system_text +=
            "\n\nTwo separate mechanisms grant tool access here. (1) A tool named in a mounted "
            "skill's instructions above becomes directly callable immediately, with no search_tools "
            "or mount_tool call needed or possible for it -- right now that means: " + unlocked_list +
            ". If a tool is in that list, just call it directly; do not search for it, and do not "
            "conclude it is unavailable if search_tools returns no match for it. (2) search_tools/"
            "mount_tool/unmount_tool cover a SEPARATE pool of scheduling and market tools that are "
            "never tied to any skill -- use those only for a capability NOT in the list above.";

        ContextContribution contribution;
        if (!combined_system_text.empty()) {
            Message m;
            m.role = role::system;
            ContentItem item;
            item.origin = content_origin::system;
            item.value = Text{std::move(combined_system_text)};
            m.content.push_back(std::move(item));
            contribution.messages.push_back(std::move(m));
        }
        contribution.messages.insert(contribution.messages.end(), session_ctx.history.begin(),
                                      session_ctx.history.end());

        // Recomputed every call, never cached: mount state (both skill-mount and tool-optimizer-mount)
        // can change mid-conversation -- see skill_tool_scoping.hpp's own top comment for why this must
        // stay recomputed at the same cadence AgentSession::run_rounds() rebuilds its ToolTable at.
        std::vector<ToolDescriptor> skill_universe_descriptors = {
            make_tool_descriptor<CheckFieldWeatherTool>(),
            make_tool_descriptor<CheckPestPressureTool>(),
            make_tool_descriptor<CheckAnimalHealthTool>(),
            make_tool_descriptor<CalculateFeedRationTool>(),
            make_tool_descriptor<CheckHarvestWeatherWindowTool>(),
            make_tool_descriptor<EstimateHarvestYieldTool>(),
            make_tool_descriptor_with_invoke<MountSkillTool>(
                [this](MountSkillArgs a, EffectContext& ctx) { return real_mount_skill(std::move(a), ctx); }),
        };
        ToolTable const skill_universe = ToolTable::from_descriptors(std::move(skill_universe_descriptors));
        auto const skill_scoped = scope_tools_to_mounted_skills(
            skill_universe, skills_.allowed_tool_names_for(mounted_skills_.all()),
            {std::string(MountSkillTool::name)});

        contribution.tools = skill_scoped.descriptors();
        contribution.tools.insert(contribution.tools.end(), optimizer_contribution->tools.begin(),
                                   optimizer_contribution->tools.end());
        co_return contribution;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }

private:
    [[nodiscard]] ToolTable build_optimizer_agent_tools() {
        std::vector<ToolDescriptor> descriptors = {
            make_tool_descriptor<ScheduleTaskTool>(),
            make_tool_descriptor<CancelTaskTool>(),
            make_tool_descriptor<GetMarketPriceTool>(),
            make_tool_descriptor<PlaceSellOrderTool>(),
            make_tool_descriptor_with_invoke<ExecuteCodeTool>(
                [this](ExecuteCodeArgs a, EffectContext& ctx) { return real_execute_code(std::move(a), ctx); }),
        };
        return ToolTable::from_descriptors(std::move(descriptors));
    }

    [[nodiscard]] result<MountSkillReply> real_mount_skill(MountSkillArgs a, EffectContext&) {
        if (a.skill_name != kCropSkillName && a.skill_name != kLivestockSkillName &&
            a.skill_name != kHarvestSkillName) {
            return std::unexpected(
                error{failure_class::contract, "unknown skill: " + a.skill_name, "skill.unknown_name"});
        }
        mounted_skill_names_log().push_back(a.skill_name);
        mounted_skills_.mount(a.skill_name);
        return MountSkillReply{true, "mounted: " + a.skill_name};
    }

    [[nodiscard]] result<ExecuteCodeReply> real_execute_code(ExecuteCodeArgs a, EffectContext& ctx) {
        execute_code_called_log() = true;
        observed_code_arg_log() = a.code;

        auto& runner = shared_farm_ops_python_runner();
        if (!runner.interpreter().ok()) {
            return std::unexpected(error{failure_class::fatal,
                                          "the embedded Python interpreter failed to initialize: " +
                                              runner.last_error(),
                                          "farm_ops.python_not_initialized"});
        }

        ExecRequest req{"python", a.code, {}};
        ExecState state{};
        auto outcome = runner.run(req, state, ctx);
        if (!outcome) return std::unexpected(outcome.error());

        ExecuteCodeReply reply;
        reply.ok = (outcome->klass == exec_outcome_class::ok);
        reply.stdout_text = outcome->stdout_text;
        reply.stderr_text = outcome->stderr_text;
        reply.result_repr = outcome->result_repr;
        observed_code_output_log() = reply.stdout_text + " " + reply.result_repr;
        return reply;
    }

    SkillsProvider<> skills_;
    MountedSkillsState mounted_skills_;
    ToolOptimizerProvider optimizer_;
};
static_assert(ContextProvider<FarmOpsProvider>);

using FarmOpsLiveSession =
    AgentSession<openai::OpenAIChatClient<InMemorySecretStore>, NoSessionState, FarmOpsProvider>;
static_assert(std::is_default_constructible_v<FarmOpsLiveSession>);

[[nodiscard]] Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{std::move(text)};
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] bool has_text(Message const& m) {
    for (ContentItem const& item : m.content) {
        if (std::holds_alternative<Text>(item.value)) return true;
    }
    return false;
}

// mount_tool/search_tools names appear only in the request wire text, not any log this file records
// directly (ToolOptimizerProvider owns that state internally) -- what this file CAN observe directly
// is whether the tools its own pool gates (get_market_price, execute_code) actually got invoked, which
// is only possible if mount_tool ran first (skill_tool_scoping's own enforcement: an unmounted tool is
// never declared, so the model could not have called it directly).

}  // namespace

int main() {
    auto const key_env = ::agentengine::pal::env_var("AGENTENGINE_OPENROUTER_API_KEY");
    if (!key_env || key_env->empty()) {
        std::fprintf(stderr,
                     "test_rt_agent_session_farm_ops_live_e2e: SKIPPED -- "
                     "AGENTENGINE_OPENROUTER_API_KEY is not set.\n"
                     "  Run tools/run-live-provider-tests.ps1, or set the variable yourself, to "
                     "exercise a real provider.\n");
        return 0;
    }

    std::string const model = env_or("AGENTENGINE_OPENROUTER_MODEL", kDefaultModel);
    std::string const host = env_or("AGENTENGINE_OPENROUTER_HOST", kDefaultHost);
    std::fprintf(stderr, "test_rt_agent_session_farm_ops_live_e2e: host=%s model=%s\n", host.c_str(),
                 model.c_str());

    InMemorySecretStore store;
    store.set(kSecretName, *key_env);
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{kSecretName, std::chrono::seconds{0}}});

    ChatClientCapabilities caps;
    caps.streaming = true;
    caps.tool_calling = true;
    caps.max_output_tokens = 1536;

    FarmOpsLiveSession session;
    // Matches `initialize()`'s own session_id below -- passed to BOTH `end_user_id` (abuse-tracking
    // only) and `session_id` (OpenRouter's own prompt-cache sticky-routing key, docs/research/
    // 2026-08-21-openrouter-session-id-header.md -- a DIFFERENT field from `end_user_id`).
    session.emplace_chat_client(host, kHttpsPort, model, SecretRef{kSecretName}, caps, store,
                                 kPathPrefix, sandbox::resolve_host, /*ca=*/std::string{},
                                 /*http_referer=*/std::string{}, /*x_title=*/kXTitle,
                                 /*end_user_id=*/std::string{"farm-ops-live-e2e-session"},
                                 /*seed=*/std::nullopt, /*transport=*/sandbox::ProviderTransport::tls,
                                 /*scan_response_format_leaks=*/false,
                                 /*session_id=*/std::string{"farm-ops-live-e2e-session"});
    // No max_turns bound: this is an ordinary, open-ended conversation, not a well-defined workflow
    // with a known step count -- a hard round limit here just turns a slow-but-still-converging
    // exchange into an artificial failure. std::nullopt makes run_rounds()'s own loop condition
    // (agent_session.hpp: `!max_turns_.has_value() || ...`) genuinely unbounded; a real workflow
    // that DOES have a well-defined shape is exactly where a max_turns budget belongs instead.
    session.initialize("farm-ops-live-e2e-session", Principal{"live-e2e-principal", ""},
                        /*token_budget=*/std::nullopt,
                        /*max_turns=*/std::nullopt);
    session.set_capabilities(&held);

    // ==================== Turn 1: an ordinary greeting -- no tool involved ==========================
    result<AgentResponse> greeting =
        drive(session.start_run(StartRun{user_message("Hi there! How are you doing today?")}));
    check(greeting.has_value(), "TURN1: the greeting round completes against the real provider");
    if (greeting.has_value()) {
        check(has_text(greeting->message), "TURN1: the greeting gets back a real text reply");
        auto stray_calls = tool_calls_of(greeting->message);
        note("TURN1 tool_calls (expected 0)", std::to_string(stray_calls.size()));
    } else {
        note("TURN1 error.code", greeting.error().code);
        note("TURN1 error.message", greeting.error().message);
    }

    // ==================== Turn 2: the crop-farming skill, phrased around the SKILL, not the tool ====
    result<AgentResponse> crop_resp = drive(session.start_run(StartRun{user_message(
        "I'm a rice farmer. My field is called 'North Paddy'. Using whatever skill you have for crop "
        "field operations, check the weather and pest pressure for North Paddy today, and tell me "
        "whether I should spray pesticide.")}));
    check(crop_resp.has_value(), "TURN2: the crop-farming round converges against the real provider");
    if (crop_resp.has_value()) {
        check(has_text(crop_resp->message), "TURN2: the converged response carries a recommendation");
    } else {
        note("TURN2 error.code", crop_resp.error().code);
        note("TURN2 error.message", crop_resp.error().message);
    }
    bool const crop_skill_mounted =
        std::find(mounted_skill_names_log().begin(), mounted_skill_names_log().end(), kCropSkillName) !=
        mounted_skill_names_log().end();
    check(crop_skill_mounted,
          "TURN2: mount_skill(crop-field-operations) was called -- the model activated the crop skill");
    check(check_weather_called_log(), "TURN2: check_field_weather was actually called");
    check(check_pest_called_log(), "TURN2: check_pest_pressure was actually called");

    // ==================== Turn 3: the livestock skill -- a DIFFERENT farm model, same round-trip ====
    result<AgentResponse> livestock_resp = drive(session.start_run(StartRun{user_message(
        "I also raise chickens. My flock is called 'Coop A' and has 200 birds. Using whatever skill "
        "you have for livestock care, check their health and calculate today's feed ration.")}));
    check(livestock_resp.has_value(),
          "TURN3: the livestock-care round converges against the real provider");
    if (livestock_resp.has_value()) {
        check(has_text(livestock_resp->message), "TURN3: the converged response carries a feeding plan");
    } else {
        note("TURN3 error.code", livestock_resp.error().code);
        note("TURN3 error.message", livestock_resp.error().message);
    }
    bool const livestock_skill_mounted =
        std::find(mounted_skill_names_log().begin(), mounted_skill_names_log().end(),
                  kLivestockSkillName) != mounted_skill_names_log().end();
    check(livestock_skill_mounted,
          "TURN3: mount_skill(livestock-care-operations) was called -- the model activated the "
          "SECOND, independently-mounted skill, proving multi-skill selection, not just repeat calls "
          "into the first skill");
    check(check_animal_health_called_log(), "TURN3: check_animal_health was actually called");
    check(calculate_feed_ration_called_log(), "TURN3: calculate_feed_ration was actually called");

    // ==================== Turn 4: the harvest-planning skill -- a THIRD independent farm model ======
    result<AgentResponse> harvest_resp = drive(session.start_run(StartRun{user_message(
        "One more thing about North Paddy -- it's 2.5 hectares of rice. Using whatever skill you have "
        "for harvest planning, tell me whether I should harvest it this week and what yield to "
        "expect.")}));
    check(harvest_resp.has_value(), "TURN4: the harvest-planning round converges against the real provider");
    if (harvest_resp.has_value()) {
        check(has_text(harvest_resp->message), "TURN4: the converged response carries a harvest plan");
    } else {
        note("TURN4 error.code", harvest_resp.error().code);
        note("TURN4 error.message", harvest_resp.error().message);
    }
    bool const harvest_skill_mounted =
        std::find(mounted_skill_names_log().begin(), mounted_skill_names_log().end(),
                  kHarvestSkillName) != mounted_skill_names_log().end();
    check(harvest_skill_mounted,
          "TURN4: mount_skill(harvest-planning-operations) was called -- the model activated the "
          "THIRD, independently-mounted skill");
    check(check_harvest_weather_called_log(), "TURN4: check_harvest_weather_window was actually called");
    check(estimate_yield_called_log(), "TURN4: estimate_harvest_yield was actually called");

    // ==================== Turn 5: the ToolOptimizerProvider pool + REAL CPython computation =========
    // Deliberately asks for BOTH an exact calculation (forcing execute_code, since the model cannot
    // guess an exact liter figure) and a market lookup (forcing get_market_price) -- neither tool is
    // declared until mount_tool runs (skill_tool_scoping's own enforcement), so a real call into
    // either one is only reachable through ToolOptimizerProvider's real search_tools/mount_tool path.
    execute_code_called_log() = false;
    observed_code_arg_log().clear();
    observed_code_output_log().clear();
    get_market_price_called_log() = false;
    observed_commodity_log().clear();

    result<AgentResponse> ops_resp = drive(session.start_run(StartRun{user_message(
        "I need to plan two things. First: I have 3 fields of 2.5 hectares each, and I'm applying 40mm "
        "of irrigation depth to each -- use your code execution tool to compute the exact total liters "
        "of water needed (1 hectare = 10000 square meters, 1mm depth over 1 square meter = 1 liter). "
        "Second: look up today's market price for rice so I know what a sale would be worth. You may "
        "need to search for and activate tools you don't see yet before you can call them.")}));
    check(ops_resp.has_value(), "TURN5: the tool-optimizer-pool round converges against the real provider");
    if (ops_resp.has_value()) {
        check(has_text(ops_resp->message), "TURN5: the converged response carries both answers");
    } else {
        note("TURN5 error.code", ops_resp.error().code);
        note("TURN5 error.message", ops_resp.error().message);
    }

    check(execute_code_called_log(),
          "TURN5: execute_code was actually called -- reachable only after a real mount_tool call "
          "into ToolOptimizerProvider's pool, since execute_code is never always-on");
    if (!observed_code_arg_log().empty()) {
        note("execute_code code argument", observed_code_arg_log());
    }
    if (!observed_code_output_log().empty()) {
        note("execute_code observed output (stdout + result_repr)", observed_code_output_log());
    }
    // 3 fields * 2.5 hectares * 10000 sq-m/hectare * 40mm-as-liters/sq-m = 3,000,000 liters -- the
    // exact figure only a REAL interpreter evaluating the model's own arithmetic expression could
    // produce; a canned tool could not have produced this specific number by construction. Checked
    // structurally against the tool's OWN real output, never the model's prose (this suite's own
    // convention). Strips thousands separators first: real Python code the model writes commonly
    // formats a large result with `f"{x:,}"`, which prints "3,000,000" rather than "3000000" --
    // still the exact real answer, just comma-grouped, so the check must not require a bare digit run.
    std::string digits_only_output;
    for (char c : observed_code_output_log()) {
        if (c != ',') digits_only_output += c;
    }
    check(execute_code_called_log() && digits_only_output.find("3000000") != std::string::npos,
          "TURN5: the real embedded CPython interpreter's own output contains the exact expected "
          "answer (3,000,000 liters) -- live, behavioural proof of genuine sandboxed computation, "
          "not a canned or guessed number");
    check(get_market_price_called_log(),
          "TURN5: get_market_price was actually called -- reachable only after a real mount_tool call "
          "into ToolOptimizerProvider's pool, since get_market_price is never always-on");
    if (!observed_commodity_log().empty()) {
        note("get_market_price commodity argument", observed_commodity_log());
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_rt_agent_session_farm_ops_live_e2e: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_agent_session_farm_ops_live_e2e: %d FAILURE(S)\n", g_failures);
    return 1;
}

#else   // AGENTENGINE_WITH_HTTPS
int main() { return 0; }
#endif  // AGENTENGINE_WITH_HTTPS
