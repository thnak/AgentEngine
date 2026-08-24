#!/usr/bin/env python3
"""Interactive, real-terminal-input CLI mirroring tests/test_rt_agent_session_farm_ops_live_e2e.cpp's
scenario -- three independently-mountable skills (crop-field-operations, livestock-care-operations,
harvest-planning-operations), a separate five-tool "optimizer pool" (schedule_task/cancel_task/
get_market_price/place_sell_order/execute_code) gated behind search_tools/mount_tool/unmount_tool,
and REAL Python code execution for execute_code (genuine exec(), not a canned stand-in).

Why this script exists: the C++ live test drives four SCRIPTED turns and reduces every observation to
a pass/fail assert. That is useful for CI but tells you nothing about WHY a run failed, and hides the
model's own reasoning/prose. This script has no asserts at all -- it is a real chat loop reading from
your terminal, printing every tool call and its result as it happens, so you can judge for yourself
whether the model is actually using the two gating mechanisms sensibly. It also isolates one variable:
this talks to OpenRouter directly through the plain `openai` Python SDK, with NO AgentEngine C++
runtime in the loop at all -- if the same kind of stalls/empty-response/tool-confusion show up here
too, that points at the model or OpenRouter, not at anything in AgentEngine's own harness.

Usage:
    C:\\Users\\thanh\\miniconda3\\python.exe tools/farm_ops_interactive_cli.py [--model MODEL]

Reads the API key from the first line of apt-openrouter.txt (repo root, git-ignored) unless
OPENROUTER_API_KEY is already set in the environment -- same convention as
tools/run-live-provider-tests.ps1 uses for the C++ tests. Never hardcode a key in this file.
"""
from __future__ import annotations

import argparse
import contextlib
import io
import itertools
import json
import math
import os
import sys
import traceback
from pathlib import Path

from openai import OpenAI

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MODEL = "stealth/ox-alpha"
CROP_SKILL = "crop-field-operations"
LIVESTOCK_SKILL = "livestock-care-operations"
HARVEST_SKILL = "harvest-planning-operations"

# Live evidence (dumps/dump-5.json, plus a cross-model check against openai/gpt-5.6-luna, both
# reproducing the identical pattern): naming two required tools was not enough -- both models
# consistently called only the tool that most directly produced the user's asked-for ANSWER and
# skipped the more confirmatory one. Wording below is deliberately more forceful and explains why
# neither tool substitutes for the other, rather than just listing both names once.
CROP_SKILL_DESC = (
    "Decide irrigation and pesticide-spraying actions for a crop field, using real weather and "
    "pest-pressure data for that field. Use this for a farmer growing crops (rice, vegetables, and "
    "similar) asking about a named field."
)
CROP_SKILL_BODY = """\
Before recommending irrigation or pesticide spraying for a named field, you MUST call BOTH
`check_field_weather` AND `check_pest_pressure` for that field's name. An answer based on only one
of the two is incomplete and wrong -- do not skip either call, and do not treat one reading as a
substitute for the other: pest pressure alone does not tell you the forecast, and the forecast
alone does not tell you pest pressure. Do not guess either value.

Reading the results:
- If the forecast shows no rain and pest pressure is moderate or high: spraying is warranted, and
  irrigation may proceed if the field needs it.
- If rain is expected: postpone both -- irrigation is redundant and spraying would wash off.
- If pest pressure is low: hold off on pesticide regardless of the forecast.

Always state both the forecast and the pest-pressure reading you actually received before giving
your recommendation."""

LIVESTOCK_SKILL_DESC = (
    "Decide feeding and health-check actions for livestock (cattle, poultry, and similar), using "
    "real herd data for a named herd or flock. Use this for a farmer raising animals asking about "
    "care for a named group."
)
LIVESTOCK_SKILL_BODY = """\
Before recommending a feeding plan for a named herd or flock, you MUST call BOTH
`check_animal_health` AND `calculate_feed_ration` (with that group's name, its animal count, and
its animal type). An answer based on only one of the two is incomplete and wrong -- do not skip
either call, and do not treat one as a substitute for the other: the ration number alone does not
tell you whether the flock is healthy, and a health status alone does not tell you how much to
feed. Do not guess either value.

Reading the results:
- If health status is anything other than healthy: recommend addressing the health issue before
  changing the feeding plan.
- Otherwise: report the computed daily ration and confirm it matches the group's normal schedule."""

# THIRD independent farm model: harvest planning, covering yet another decision (timing + yield)
# that structurally needs two tools together, same as the two skills above.
HARVEST_SKILL_DESC = (
    "Decide whether to harvest a named field this week and what yield to expect, using real "
    "weather-window and yield-estimate data for that field. Use this for a farmer growing crops "
    "asking about harvest timing or expected yield for a named field."
)
HARVEST_SKILL_BODY = """\
Before recommending a harvest timing or reporting an expected yield for a named field, you MUST
call BOTH `check_harvest_weather_window` AND `estimate_harvest_yield` for that field. An answer
based on only one of the two is incomplete and wrong: the weather window tells you WHEN it is safe
to harvest, and the yield estimate tells you HOW MUCH to expect -- neither substitutes for the
other. Do not guess either value.

Reading the results:
- If the weather window is not dry enough: recommend waiting, regardless of the yield estimate.
- If the weather window is dry enough: recommend harvesting now, and report the estimated yield."""

SKILL_ALLOWED_TOOLS = {
    CROP_SKILL: ["check_field_weather", "check_pest_pressure"],
    LIVESTOCK_SKILL: ["check_animal_health", "calculate_feed_ration"],
    HARVEST_SKILL: ["check_harvest_weather_window", "estimate_harvest_yield"],
}
SKILL_BODIES = {CROP_SKILL: CROP_SKILL_BODY, LIVESTOCK_SKILL: LIVESTOCK_SKILL_BODY,
                 HARVEST_SKILL: HARVEST_SKILL_BODY}
SKILL_DESCS = {CROP_SKILL: CROP_SKILL_DESC, LIVESTOCK_SKILL: LIVESTOCK_SKILL_DESC,
               HARVEST_SKILL: HARVEST_SKILL_DESC}

OPTIMIZER_POOL_TOOLS = ["schedule_task", "cancel_task", "get_market_price", "place_sell_order",
                        "execute_code"]

MANAGEMENT_TOOL_SCHEMAS = [
    {
        "type": "function",
        "function": {
            "name": "mount_skill",
            "description": (
                "Activate a skill you've seen advertised by name and description, so its full "
                "instructions become part of your context and any tools it names become callable -- "
                "starting next turn."
            ),
            "parameters": {
                "type": "object",
                "properties": {"skill_name": {"type": "string"}},
                "required": ["skill_name"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "search_tools",
            "description": (
                "Search a SEPARATE pool of scheduling/market/compute tools (not skill-gated) by "
                "name/description keyword match. Call mount_tool with a name from the results before "
                "you can actually invoke it."
            ),
            "parameters": {
                "type": "object",
                "properties": {"query": {"type": "string"}},
                "required": ["query"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "mount_tool",
            "description": "Activate a tool from that separate pool by name, callable starting next turn.",
            "parameters": {
                "type": "object",
                "properties": {"name": {"type": "string"}},
                "required": ["name"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "unmount_tool",
            "description": "Deactivate a currently-mounted pool tool, shrinking your declared surface.",
            "parameters": {
                "type": "object",
                "properties": {"name": {"type": "string"}},
                "required": ["name"],
            },
        },
    },
]

DOMAIN_TOOL_SCHEMAS = {
    "check_field_weather": {
        "type": "function",
        "function": {
            "name": "check_field_weather",
            "description": "Gets today's local weather forecast for a named field.",
            "parameters": {
                "type": "object",
                "properties": {"field_name": {"type": "string"}},
                "required": ["field_name"],
            },
        },
    },
    "check_pest_pressure": {
        "type": "function",
        "function": {
            "name": "check_pest_pressure",
            "description": "Gets today's measured pest pressure for a named field.",
            "parameters": {
                "type": "object",
                "properties": {"field_name": {"type": "string"}},
                "required": ["field_name"],
            },
        },
    },
    "check_animal_health": {
        "type": "function",
        "function": {
            "name": "check_animal_health",
            "description": "Gets today's health status for a named herd or flock.",
            "parameters": {
                "type": "object",
                "properties": {"herd_name": {"type": "string"}},
                "required": ["herd_name"],
            },
        },
    },
    "calculate_feed_ration": {
        "type": "function",
        "function": {
            "name": "calculate_feed_ration",
            "description": "Computes today's total feed ration (kg) for a named herd or flock.",
            "parameters": {
                "type": "object",
                "properties": {
                    "herd_name": {"type": "string"},
                    "animal_count": {"type": "integer"},
                    "animal_type": {"type": "string"},
                },
                "required": ["herd_name", "animal_count", "animal_type"],
            },
        },
    },
    "check_harvest_weather_window": {
        "type": "function",
        "function": {
            "name": "check_harvest_weather_window",
            "description": "Checks whether the coming days are dry enough to safely harvest a named field.",
            "parameters": {
                "type": "object",
                "properties": {"field_name": {"type": "string"}},
                "required": ["field_name"],
            },
        },
    },
    "estimate_harvest_yield": {
        "type": "function",
        "function": {
            "name": "estimate_harvest_yield",
            "description": "Estimates the harvest yield (kg) for a named field, given its area (hectares) and crop type.",
            "parameters": {
                "type": "object",
                "properties": {
                    "field_name": {"type": "string"},
                    "hectares": {"type": "number"},
                    "crop_type": {"type": "string"},
                },
                "required": ["field_name", "hectares", "crop_type"],
            },
        },
    },
}

POOL_TOOL_SCHEMAS = {
    "schedule_task": {
        "type": "function",
        "function": {
            "name": "schedule_task",
            "description": "Schedules a farm task to happen in a given number of days from today.",
            "parameters": {
                "type": "object",
                "properties": {
                    "description": {"type": "string"},
                    "due_in_days": {"type": "integer"},
                },
                "required": ["description", "due_in_days"],
            },
        },
    },
    "cancel_task": {
        "type": "function",
        "function": {
            "name": "cancel_task",
            "description": "Cancels a previously scheduled farm task by its description text.",
            "parameters": {
                "type": "object",
                "properties": {"description": {"type": "string"}},
                "required": ["description"],
            },
        },
    },
    "get_market_price": {
        "type": "function",
        "function": {
            "name": "get_market_price",
            "description": "Gets today's local market price per kilogram for a named commodity.",
            "parameters": {
                "type": "object",
                "properties": {"commodity": {"type": "string"}},
                "required": ["commodity"],
            },
        },
    },
    "place_sell_order": {
        "type": "function",
        "function": {
            "name": "place_sell_order",
            "description": "Places a sell order for a quantity (kg) of a commodity at today's price.",
            "parameters": {
                "type": "object",
                "properties": {
                    "commodity": {"type": "string"},
                    "quantity_kg": {"type": "number"},
                },
                "required": ["commodity", "quantity_kg"],
            },
        },
    },
    "execute_code": {
        "type": "function",
        "function": {
            "name": "execute_code",
            "description": (
                "Runs a snippet of REAL Python code in a sandboxed interpreter and returns what it "
                "printed (the math module is available). Use this for any calculation you need to "
                "get exactly right -- irrigation volumes, feed totals, sale values -- rather than "
                "computing it yourself."
            ),
            "parameters": {
                "type": "object",
                "properties": {"code": {"type": "string"}},
                "required": ["code"],
            },
        },
    },
}

FEED_RATE_PER_ANIMAL = {"chicken": 0.12, "poultry": 0.12, "cattle": 11.0, "cow": 11.0, "pig": 2.3}
PRICE_PER_KG = {"rice": 0.42, "chicken": 3.10, "poultry": 3.10, "pork": 2.75, "pig": 2.75}
YIELD_RATE_PER_HECTARE = {"rice": 4500.0, "vegetables": 12000.0, "vegetable": 12000.0}


class FarmOpsState:
    def __init__(self) -> None:
        self.mounted_skills: set[str] = set()
        self.mounted_pool_tools: set[str] = set()
        self.scheduled_tasks: list[str] = []

    # ---- tool implementations, printed as they run so you can watch what actually happened -------
    def mount_skill(self, skill_name: str) -> dict:
        if skill_name not in SKILL_ALLOWED_TOOLS:
            return {"ok": False, "message": f"unknown skill: {skill_name}"}
        self.mounted_skills.add(skill_name)
        return {"ok": True, "message": f"mounted: {skill_name}"}

    def search_tools(self, query: str) -> dict:
        # Token-overlap match, not a whole-phrase substring match: a real run showed the model try
        # "code execution compute calculation", "python calculator math", "run code script compute
        # liters", etc. for execute_code -- none of those exact PHRASES appear verbatim in its name or
        # description, so a whole-query substring check (matching this file's earlier version, and the
        # real C++ ToolOptimizerProvider's own real_search_tools() heuristic) returned empty every
        # time, and the model gave up rather than ever finding it. Matching if ANY query word appears
        # is what a user actually expects "search" to mean.
        words = [w for w in query.lower().split() if w]
        names = []
        for n in OPTIMIZER_POOL_TOOLS:
            haystack = n.lower() + " " + POOL_TOOL_SCHEMAS[n]["function"]["description"].lower()
            if not words or any(w in haystack for w in words):
                names.append(n)
        return {"names": names}

    def mount_tool(self, name: str) -> dict:
        if name not in OPTIMIZER_POOL_TOOLS:
            return {"ok": False, "message": f"unknown tool: {name}"}
        self.mounted_pool_tools.add(name)
        return {"ok": True, "message": f"mounted: {name}"}

    def unmount_tool(self, name: str) -> dict:
        self.mounted_pool_tools.discard(name)
        return {"ok": True, "message": f"unmounted: {name}"}

    def check_field_weather(self, field_name: str) -> dict:
        return {
            "forecast": "Clear skies, no rain expected for 3 days, around 34C.",
            "advice": f"No rain in the forecast for {field_name} -- irrigation may proceed if the "
                      "field needs it. Check pest pressure before deciding on spraying.",
        }

    def check_pest_pressure(self, field_name: str) -> dict:
        # Deliberately says NOTHING about weather/forecast -- a captured live transcript
        # (dumps/dump-5.json) showed the model treating an earlier version's "given the dry forecast"
        # wording as if it had already learned the forecast, never calling check_field_weather at
        # all, then looping on repeated redundant check_pest_pressure calls instead.
        return {
            "level": "high",
            "recommendation": f"Aphid pressure on {field_name} is high, which alone warrants "
                               "pesticide treatment. This says nothing about the weather -- check "
                               "that separately.",
        }

    def check_animal_health(self, herd_name: str) -> dict:
        return {"status": "healthy", "notes": f"No signs of illness in {herd_name} -- proceed with "
                                               "standard feeding."}

    def calculate_feed_ration(self, herd_name: str, animal_count: int, animal_type: str) -> dict:
        rate = FEED_RATE_PER_ANIMAL.get(animal_type, 1.0)
        total = animal_count * rate
        return {"ration_kg_per_day": total,
                 "notes": f"{herd_name}'s ration is based on {animal_count} {animal_type}(s) at the "
                          "standard per-animal rate."}

    def check_harvest_weather_window(self, field_name: str) -> dict:
        return {"dry_enough": True,
                "advice": f"The next 5 days for {field_name} are dry -- safe to harvest now."}

    def estimate_harvest_yield(self, field_name: str, hectares: float, crop_type: str) -> dict:
        rate = YIELD_RATE_PER_HECTARE.get(crop_type, 3000.0)
        total = hectares * rate
        return {"estimated_yield_kg": total,
                 "notes": f"{field_name}'s estimated yield is based on {hectares} hectares of "
                          f"{crop_type} at the standard per-hectare rate."}

    def schedule_task(self, description: str, due_in_days: int) -> dict:
        self.scheduled_tasks.append(f"{description} (in {due_in_days} day(s))")
        return {"ok": True, "message": f"scheduled: {description}"}

    def cancel_task(self, description: str) -> dict:
        for t in list(self.scheduled_tasks):
            if description in t:
                self.scheduled_tasks.remove(t)
                return {"ok": True, "message": f"cancelled: {description}"}
        return {"ok": False, "message": f"no scheduled task matched: {description}"}

    def get_market_price(self, commodity: str) -> dict:
        return {"commodity": commodity, "price_per_kg": PRICE_PER_KG.get(commodity, 1.0)}

    def place_sell_order(self, commodity: str, quantity_kg: float) -> dict:
        total = quantity_kg * PRICE_PER_KG.get(commodity, 1.0)
        return {"ok": True, "message": f"sell order placed: {quantity_kg}kg of {commodity}",
                "total_value": total}

    def execute_code(self, code: str) -> dict:
        # REAL execution -- genuine CPython exec(), not a canned stand-in, mirroring the C++ test's
        # own use of native_jail::PythonRunner. Deliberately light sandboxing (a restricted globals
        # dict, no builtins beyond a small allowlist) -- this script is a manual verification tool
        # run by you at your own terminal, not a production security boundary.
        safe_builtins = {"print": print, "range": range, "len": len, "sum": sum, "min": min,
                          "max": max, "round": round, "abs": abs, "int": int, "float": float,
                          "str": str}
        exec_globals = {"__builtins__": safe_builtins, "math": math}
        stdout_buf = io.StringIO()
        stderr_text = ""
        ok = True
        try:
            with contextlib.redirect_stdout(stdout_buf):
                exec(code, exec_globals)
        except Exception:
            ok = False
            stderr_text = traceback.format_exc()
        return {"ok": ok, "stdout_text": stdout_buf.getvalue(), "stderr_text": stderr_text}


TOOL_IMPLS = {
    "mount_skill": lambda s, a: s.mount_skill(**a),
    "search_tools": lambda s, a: s.search_tools(**a),
    "mount_tool": lambda s, a: s.mount_tool(**a),
    "unmount_tool": lambda s, a: s.unmount_tool(**a),
    "check_field_weather": lambda s, a: s.check_field_weather(**a),
    "check_pest_pressure": lambda s, a: s.check_pest_pressure(**a),
    "check_animal_health": lambda s, a: s.check_animal_health(**a),
    "calculate_feed_ration": lambda s, a: s.calculate_feed_ration(**a),
    "check_harvest_weather_window": lambda s, a: s.check_harvest_weather_window(**a),
    "estimate_harvest_yield": lambda s, a: s.estimate_harvest_yield(**a),
    "schedule_task": lambda s, a: s.schedule_task(**a),
    "cancel_task": lambda s, a: s.cancel_task(**a),
    "get_market_price": lambda s, a: s.get_market_price(**a),
    "place_sell_order": lambda s, a: s.place_sell_order(**a),
    "execute_code": lambda s, a: s.execute_code(**a),
}


def build_system_message(state: FarmOpsState) -> dict:
    lines = [f"{name}: {SKILL_DESCS[name]}" for name in SKILL_ALLOWED_TOOLS]
    for name in state.mounted_skills:
        lines.append(f"\nMounted skill '{name}':\n{SKILL_BODIES[name]}")
    # Live evidence (dumps/dump-2.json, dump-5.json): stating the two-mechanism policy in the
    # abstract was not enough -- the model repeatedly searched for a skill-unlocked tool, got an
    # empty result from search_tools (correct -- that pool never includes skill-gated tools), and
    # wrongly concluded the tool didn't exist at all, even though it was already directly declared.
    # Naming the currently-callable tools concretely removes that inference step.
    unlocked = ["mount_skill"]
    for skill_name in state.mounted_skills:
        unlocked.extend(SKILL_ALLOWED_TOOLS[skill_name])
    lines.append(
        "\n\nTwo separate mechanisms grant tool access here. (1) A tool named in a mounted skill's "
        "instructions above becomes directly callable immediately, with no search_tools or mount_tool "
        f"call needed or possible for it -- right now that means: {', '.join(unlocked)}. If a tool is "
        "in that list, just call it directly; do not search for it, and do not conclude it is "
        "unavailable if search_tools returns no match for it. (2) search_tools/mount_tool/unmount_tool "
        "cover a SEPARATE pool of scheduling and market tools that are never tied to any skill -- use "
        "those only for a capability NOT in the list above."
    )
    return {"role": "system", "content": "\n".join(lines)}


def current_tools(state: FarmOpsState) -> list[dict]:
    tools = list(MANAGEMENT_TOOL_SCHEMAS)
    for skill_name in state.mounted_skills:
        for tool_name in SKILL_ALLOWED_TOOLS[skill_name]:
            tools.append(DOMAIN_TOOL_SCHEMAS[tool_name])
    for tool_name in state.mounted_pool_tools:
        tools.append(POOL_TOOL_SCHEMAS[tool_name])
    return tools


def read_api_key() -> str:
    env_key = os.environ.get("OPENROUTER_API_KEY") or os.environ.get("AGENTENGINE_OPENROUTER_API_KEY")
    if env_key:
        return env_key
    key_file = REPO_ROOT / "apt-openrouter.txt"
    if key_file.exists():
        first_line = key_file.read_text(encoding="utf-8").splitlines()[0].strip()
        if first_line:
            return first_line
    print(f"error: no API key found (set OPENROUTER_API_KEY, or put it as the first line of "
          f"{key_file})", file=sys.stderr)
    sys.exit(1)


def print_tool_call(name: str, args: dict, result: dict) -> None:
    print(f"  \033[36m[tool] {name}({json.dumps(args, ensure_ascii=False)})\033[0m")
    result_text = json.dumps(result, ensure_ascii=False)
    if len(result_text) > 400:
        result_text = result_text[:400] + "...(truncated)"
    print(f"  \033[36m  -> {result_text}\033[0m")


def run_turn(client: OpenAI, model: str, state: FarmOpsState, messages: list[dict],
             max_rounds: int | None = None) -> None:
    # Unbounded by default: this is an ordinary, open-ended conversation, not a well-defined workflow
    # with a known step count, and a hard round cap just turns a slow-but-still-converging exchange
    # into an artificial stop. You are watching this run live and can Ctrl+C at any point, so there is
    # no need for the harness itself to guess a budget on your behalf -- pass --max-rounds for a real
    # bounded-workflow scenario where a limit actually belongs.
    rounds = itertools.count() if max_rounds is None else range(max_rounds)
    for round_index in rounds:
        request_messages = [build_system_message(state)] + messages
        tools = current_tools(state)
        try:
            resp = client.chat.completions.create(
                model=model, messages=request_messages, tools=tools, tool_choice="auto",
            )
        except Exception as exc:
            # A silently-swallowed exception here is exactly the "user turn right after a tool call,
            # no assistant summary in between" pattern that made a dumped transcript look inexplicable
            # -- the request for the NEXT round (after tool results were already appended) failed, and
            # nothing recorded that fact into `messages`. Print the full type+message AND leave a
            # marker inside the transcript itself, so a later dump shows exactly what happened instead
            # of a silent gap.
            print(f"  \033[31m[error] request failed ({type(exc).__name__}): {exc}\033[0m")
            messages.append({
                "role": "system",
                "content": f"[harness note: the request for this round failed -- "
                           f"{type(exc).__name__}: {exc} -- no assistant reply was produced this "
                           f"round, the turn ended here without a summary]",
            })
            return

        choice = resp.choices[0]
        msg = choice.message
        assistant_entry = {"role": "assistant", "content": msg.content or ""}
        if msg.tool_calls:
            assistant_entry["tool_calls"] = [
                {"id": tc.id, "type": "function",
                 "function": {"name": tc.function.name, "arguments": tc.function.arguments}}
                for tc in msg.tool_calls
            ]
        messages.append(assistant_entry)

        if msg.content:
            print(f"\n\033[32massistant:\033[0m {msg.content}")

        if not msg.tool_calls:
            if not msg.content:
                print("  \033[33m[note] assistant returned no text and no tool calls this round "
                      "-- an empty turn, printed here rather than hidden behind an assert\033[0m")
            return

        for tc in msg.tool_calls:
            name = tc.function.name
            try:
                args = json.loads(tc.function.arguments or "{}")
            except json.JSONDecodeError as exc:
                result = {"error": f"could not parse arguments: {exc}"}
                print_tool_call(name, {"_raw": tc.function.arguments}, result)
            else:
                impl = TOOL_IMPLS.get(name)
                if impl is None:
                    result = {"error": f"unknown tool: {name}"}
                else:
                    result = impl(state, args)
                print_tool_call(name, args, result)
            messages.append({
                "role": "tool", "tool_call_id": tc.id, "content": json.dumps(result, ensure_ascii=False),
            })

        if max_rounds is not None and round_index == max_rounds - 1:
            print(f"  \033[33m[note] hit max_rounds ({max_rounds}) without a final text reply this turn\033[0m")


def next_dump_path() -> Path:
    dumps_dir = REPO_ROOT / "dumps"
    dumps_dir.mkdir(exist_ok=True)
    existing = sorted(dumps_dir.glob("dump-*.json"))
    next_n = 1
    for p in existing:
        try:
            n = int(p.stem.split("-", 1)[1])
        except (IndexError, ValueError):
            continue
        next_n = max(next_n, n + 1)
    return dumps_dir / f"dump-{next_n}.json"


def write_dump(path: Path, messages: list[dict]) -> None:
    # Overwritten after every turn (not just at exit) so a hang or a Ctrl+C mid-turn still leaves a
    # transcript on disk reflecting everything up to the last completed round -- the same reason the
    # harness-note-on-failure fix above exists: a partial/failed session should still be inspectable.
    path.write_text(json.dumps(messages, indent=2, ensure_ascii=False), encoding="utf-8")


def main() -> None:
    # Windows consoles default to a legacy codepage (cp1252) that cannot encode most emoji the model
    # may send back -- reconfigure stdout/stderr to UTF-8 with replacement rather than crashing mid-turn.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", default=os.environ.get("AGENTENGINE_OPENROUTER_MODEL", DEFAULT_MODEL))
    parser.add_argument("--max-rounds", type=int, default=None,
                         help="Cap tool-call rounds per turn. Omit for unbounded (the default) -- "
                              "only set this for a well-defined workflow that actually has a known "
                              "step count.")
    args = parser.parse_args()

    api_key = read_api_key()
    client = OpenAI(base_url="https://openrouter.ai/api/v1", api_key=api_key, timeout=60.0)

    state = FarmOpsState()
    messages: list[dict] = []
    dump_path = next_dump_path()

    print(f"farm_ops_interactive_cli: model={args.model}")
    print(f"transcript will be written to: {dump_path}")
    print("Type a message and press Enter. Ctrl+C or an empty line + Ctrl+D to quit.")
    print("Try: \"I'm a rice farmer, field 'North Paddy' -- should I spray today?\"")
    print("Then: \"I raise 200 chickens in Coop A -- check their health and feed ration.\"")
    print("Then: \"Should I harvest North Paddy (2.5ha of rice) this week, and what yield to expect?\"")
    print("Then: \"Compute 3 fields of 2.5ha at 40mm irrigation in liters, and check rice price.\"\n")

    try:
        while True:
            try:
                user_text = input("\033[34myou:\033[0m ").strip()
            except EOFError:
                break
            if not user_text:
                continue
            messages.append({"role": "user", "content": user_text})
            run_turn(client, args.model, state, messages, max_rounds=args.max_rounds)
            write_dump(dump_path, [build_system_message(state)] + messages)
    except KeyboardInterrupt:
        pass
    write_dump(dump_path, [build_system_message(state)] + messages)
    print(f"\ntranscript written to: {dump_path}")
    print("bye.")


if __name__ == "__main__":
    main()
