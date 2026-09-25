"""ADR-182 C9 contract run: the MCP Inspector CLI against agentengine_test_driver.

Usage: python tools/test_driver/c9_inspector_contract.py <path-to-agentengine_test_driver> [scenarios-root]

Needs node/npx and network access for the first npx download, so it is a manual contract run rather
than a ctest. Each Inspector CLI command starts a fresh driver process, so a multi-call approve round
trip cannot span invocations; the approve round trip runs inside one tools/call via scenario_replay
of the checked-in live_gated_approve scenario (suspend -> approve -> tool runs -> final text).
Exit code 0 when every check passes.
"""
import json
import os
import subprocess
import sys
import tempfile

INSPECTOR = "@modelcontextprotocol/inspector@latest"


def inspector(config, *cli_args):
    # Server args go through a config file: the Inspector CLI parses any --flag after the server
    # command as its own option, so `driver --scenarios-root X` on the command line loses the flag.
    cmd = ["npx", "-y", INSPECTOR, "--cli", "--config", config, "--server", "driver", *cli_args]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=300, shell=(sys.platform == "win32"))
    if proc.returncode != 0:
        raise SystemExit(f"FAIL: inspector exited {proc.returncode}: {' '.join(cli_args)}\n{proc.stderr[-2000:]}")
    start = proc.stdout.find("{")
    return json.loads(proc.stdout[start:])


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    driver = os.path.abspath(sys.argv[1])
    root = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "tests", "scenarios")
    fd, config = tempfile.mkstemp(suffix=".json")
    with os.fdopen(fd, "w") as f:
        json.dump({"mcpServers": {"driver": {"command": driver, "args": ["--scenarios-root", root]}}}, f)
    try:
        return run(config)
    finally:
        os.remove(config)


def run(config):
    failures = []

    def check(ok, what):
        print(("ok:   " if ok else "FAIL: ") + what)
        if not ok:
            failures.append(what)

    l1 = inspector(config, "--method", "tools/list")
    l2 = inspector(config, "--method", "tools/list")
    names = [t["name"] for t in l1.get("tools", [])]
    check(l1 == l2, f"tools/list is identical across two runs ({len(names)} tools)")
    check("scenario_replay" in names and "session_start" in names, "tools/list names the driver tools")

    fx = inspector(config, "--method", "tools/call", "--tool-name", "fixtures_list")
    check(not fx.get("isError") and "gated_echo" in json.dumps(fx), "tools/call fixtures_list succeeds")

    rp = inspector(config, "--method", "tools/call", "--tool-name", "scenario_replay",
                   "--tool-arg", "name=live_gated_approve")
    report = rp.get("structuredContent", {})
    check(not rp.get("isError") and report.get("passed") is True and report.get("events_compared", 0) > 0,
          f"approve round trip: scenario_replay live_gated_approve passes ({report.get('events_compared')} events)")

    print("C9: PASS" if not failures else f"C9: FAIL ({len(failures)})")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
