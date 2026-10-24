import sys, os, re
sys.argv = [sys.argv[0], sys.argv[1]]
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "cut_edges.py")).read().split("edges = defaultdict(list)")[0]
exec(src)
from collections import Counter
c = Counter(); ex = {}
for f in files:
    if not moves(f) or f.startswith("tests"): continue
    for n, line in enumerate(open(os.path.join(root, f), encoding="utf-8", errors="replace"), 1):
        m = inc.match(line)
        if not m: continue
        t = resolve(f, m.group(1))
        if t and not moves(t) and t.startswith("src/"):
            c[t] += 1
for t, k in sorted(c.items(), key=lambda x: -x[1]): print(f"{k:3} {t}")
print("distinct non-installed engine files the moved code needs:", len(c))
