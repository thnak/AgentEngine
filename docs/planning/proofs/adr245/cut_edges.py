"""Count #include edges from files that stay in AgentEngine to files that move (ADR-245 evidence).

usage: python -I cut_edges.py <repo-root>
"""
import os, re, sys
from collections import defaultdict

root = sys.argv[1]
JAIL = ("native_jail_backend linux_native_jail_backend app_container_profile cgroup_limits "
        "seccomp_filter native_jail_win32_helpers native_jail_handle_relay "
        "jailed_worker_rpc python_worker_main pdf_worker_main python_lockdown python_worker_mediation "
        ""
        "relay_base64 python_runner mediated_python_runner").split()

def moves(p):
    p = p.replace("\\", "/")
    if p.startswith("src/backends/kata/") or p.startswith("tests/backends/kata/"):
        return "kata"
    if p.startswith("src/backends/native_jail/"):
        base = os.path.splitext(os.path.basename(p))[0]
        return "jail" if base in JAIL else None
    for s in ("docker_execution_surface", "docker_persistent_shell_surface", "containerd_execution_surface"):
        if os.path.basename(p).startswith(s):
            return "surface"
    return None

files = []
for top in ("include", "src", "tools", "tests", "examples"):
    for d, _, fs in os.walk(os.path.join(root, top)):
        for f in fs:
            if f.endswith((".hpp", ".cpp", ".h", ".c")):
                files.append(os.path.relpath(os.path.join(d, f), root).replace("\\", "/"))
fileset = set(files)
inc = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')

def resolve(src, target):
    for base in (os.path.dirname(src), "include", "src", ""):
        cand = os.path.normpath(os.path.join(base, target)).replace("\\", "/")
        if cand in fileset:
            return cand
    return None

edges = defaultdict(list)
for f in files:
    if moves(f):
        continue
    with open(os.path.join(root, f), encoding="utf-8", errors="replace") as fh:
        for n, line in enumerate(fh, 1):
            m = inc.match(line)
            if not m:
                continue
            t = resolve(f, m.group(1))
            if t and moves(t):
                edges[(f.split("/")[0] + "/" + f.split("/")[1] if f.count("/") > 1 else f.split("/")[0], moves(t))].append(f"{f}:{n} -> {t}")

total = sum(len(v) for v in edges.values())
print(f"stay->move include edges: {total}")
for k in sorted(edges):
    print(f"  {k[0]:<28} -> {k[1]:<8} {len(edges[k])}")
print()
for k in sorted(edges):
    if not k[0].startswith("tests"):
        for e in edges[k]:
            print("   ", e)
