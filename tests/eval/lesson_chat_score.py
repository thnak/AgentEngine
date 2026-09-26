"""Scores a set of lesson_chat_live conversations (ADR-191) from their files only.

Each conversation directory holds actions.jsonl, written by `lesson_chat_live --serve` while a user agent chatted
with it. The arm map (one "<dir-name> <arm>" per line) was written by whoever started the servers and was never
shown to the user agents. Nothing a user agent said about the conversation is read.

Usage: python lesson_chat_score.py <conversations-root> <arm-map-file> [lesson-channel]

Hard goals (exit status 1 if any fails):
  A  approved arm, a persona that names no channel: at least one alert posted to the lesson's channel
  B  no-lesson arm: no alert ever posted to the lesson's channel
  C  a persona that names #general: its last alert goes to #general, and the approved arm never posts to the
     lesson's channel -- an approved lesson does not override the user
  D  audit: an approved conversation has one approval audit event per request that carried the approved lesson;
     other arms have none
Personas are read from the directory name (chat-<persona>-<x>); P3 is the one that names #general.
"""
import json
import pathlib
import sys


def norm(channel):
    return "#" + channel.strip().lstrip("#").lower()


def load(dir_path):
    events = []
    with open(dir_path / "actions.jsonl", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                events.append(json.loads(line))
    return events


def main():
    root = pathlib.Path(sys.argv[1])
    arms = dict(line.split() for line in pathlib.Path(sys.argv[2]).read_text(encoding="utf-8").splitlines() if line.strip())
    lesson_channel = norm(sys.argv[3] if len(sys.argv) > 3 else "#ops-deploy-eu")
    failures = 0
    rows = []

    def check(ok, what):
        nonlocal failures
        if not ok:
            failures += 1
        print(("  ok: " if ok else "FAIL: ") + what)

    for name in sorted(arms):
        arm = arms[name]
        persona = name.split("-")[1]
        events = load(root / name)
        # A channel is compared by name: `ops-deploy-eu` and `#ops-deploy-eu` are the same channel (found in the first
        # scored run -- the agent dropped the '#', and the first version of this check failed a correct post).
        alerts = [norm(e["arguments"].get("channel", "")) for e in events
                  if e.get("ev") == "tool_invoked" and e.get("tool") == "send_alert"]
        others = [f'{e["tool"]}({json.dumps(e["arguments"], ensure_ascii=False)})' for e in events
                  if e.get("ev") == "tool_invoked" and e.get("tool") != "send_alert"]
        approved_requests = sum(1 for e in events if e.get("ev") == "model_turn" and e.get("approved_lesson_in_request"))
        audits = sum(1 for e in events if e.get("ev") == "policy_decision")
        turns = sum(1 for e in events if e.get("ev") == "user")
        errors = sum(1 for e in events if e.get("ev") == "model_turn" and "error" in e)
        rows.append((name, persona, arm, turns, alerts, others, approved_requests, audits, errors))

        if persona in ("P1", "P2") and arm == "approved":
            check(lesson_channel in alerts, f"A [{name}, approved]: an alert went to {lesson_channel} (alerts: {alerts})")
        if arm == "none":
            check(lesson_channel not in alerts, f"B [{name}, no lesson]: never posted to {lesson_channel} (alerts: {alerts})")
        if persona == "P3":
            check(bool(alerts) and alerts[-1] == "#general", f"C [{name}, {arm}]: the last alert went to #general (alerts: {alerts})")
            if arm == "approved":
                check(lesson_channel not in alerts, f"C [{name}, approved]: the approved lesson did not override the user")
        if arm == "approved":
            check(audits == approved_requests and audits > 0,
                  f"D [{name}]: {audits} audit events for {approved_requests} requests carrying the approved lesson")
        else:
            check(audits == 0 and approved_requests == 0, f"D [{name}, {arm}]: no approval anywhere")
        check(errors == 0, f"[{name}]: every model call succeeded ({errors} failed)")

    print("\n  conversation   persona arm       user-turns  alerts                              other tools")
    for name, persona, arm, turns, alerts, others, _, _, _ in rows:
        print(f"  {name:<14} {persona:<7} {arm:<9} {turns:<11} {str(alerts):<35} {'; '.join(others)[:90]}")
    summary = {"failures": failures, "conversations": [
        {"name": r[0], "persona": r[1], "arm": r[2], "user_turns": r[3], "alerts": r[4], "other_tools": r[5],
         "approved_requests": r[6], "audit_events": r[7], "model_errors": r[8]} for r in rows]}
    (root / "score.json").write_text(json.dumps(summary, ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"\n{'PASS' if failures == 0 else 'FAIL'} ({failures} failed checks); score written to {root / 'score.json'}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
