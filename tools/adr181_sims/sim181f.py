"""ADR-195 round-3 fixes. Needs numpy. Fixed seeds.

D1  the SPECIFIED steering detector: task-stratified permutation test on the sum over (task, slot) chi-square,
    one joint test (no Holm), T vs B only (no B'); m slots, per-task steers, opposite-direction steers, undelivered dilution.
W2  winner's curse re-run with dev->shard shrink and small real effects, and the corrected eligibility rule
    (lower one-sided CI bound above a practical margin, not above 0).
T1  Tier-1 screen power: 30 tasks x K=5 x 2 arms (300 runs) vs gross harm, and a probe follow-rate screen.
"""
import numpy as np
from math import comb

rng = np.random.default_rng(1815)

# ---------------------------------------------------------------- D1
def chi_stat(ct, cb):
    # ct, cb: (..., strata, slots, values); joint statistic = sum of two-sample chi-square over every (stratum, slot)
    tot = ct + cb
    nt = ct.sum(axis=-1, keepdims=True); nb = cb.sum(axis=-1, keepdims=True); n = nt + nb
    with np.errstate(divide='ignore', invalid='ignore'):
        et = tot * nt / n; eb = tot * nb / n
        s = np.where(et > 0, (ct - et) ** 2 / et, 0.0) + np.where(eb > 0, (cb - eb) ** 2 / eb, 0.0)
    return np.nansum(s, axis=(-3, -2, -1))

def simulate(T, K, m, base, steer, undeliv=0.0):
    V = len(base)
    out = rng.choice(V, p=base, size=(2, T, K, m))
    for (t, s_), (_, to, q) in steer.items():
        hit = rng.random(K) < q
        if undeliv > 0: hit &= rng.random(K) >= undeliv
        out[1, t, hit, s_] = to
    return out

def strat_perm_p(out, V, B=400, pooled=False):
    oh = np.eye(V)[out]                      # (2,T,K,m,V)
    def counts(o0, o1):
        ct = o1.sum(axis=1); cb = o0.sum(axis=1)     # (T,m,V)
        if pooled: ct = ct.sum(axis=0, keepdims=True); cb = cb.sum(axis=0, keepdims=True)
        return ct, cb
    ct, cb = counts(oh[0], oh[1]); obs = float(chi_stat(ct, cb))
    T, K = out.shape[1], out.shape[2]
    ge = 0
    for _ in range(B):
        flip = (rng.random((T, K)) < 0.5)[:, :, None, None]
        o0 = np.where(flip, oh[1], oh[0]); o1 = np.where(flip, oh[0], oh[1])
        c2t, c2b = counts(o0, o1)
        if float(chi_stat(c2t, c2b)) >= obs - 1e-9: ge += 1
    return (ge + 1) / (B + 1)

base = np.array([.40, .30, .15, .10, .03, .02])
V = len(base)
R = 150
print("== D1 specified detector: task-STRATIFIED joint permutation, T vs B only, no B'. 10 tasks x K=30/arm (N=300/arm), m slots per task")
def rate(m, steer, undeliv=0.0, pooled=False, reps=R):
    return sum(strat_perm_p(simulate(10, 30, m, base, steer, undeliv), V, B=300, pooled=pooled) < 0.05 for _ in range(reps)) / reps
for m in (1, 10):
    print(f" m={m:2d} slots  no steer (false flag):            {rate(m, {}):.2f}")
    st_all = {(t, 0): (None, 5, 0.10) for t in range(10)}
    print(f" m={m:2d} slots  10% steer, every task, slot 0:      stratified {rate(m, st_all):.2f}   pooled {rate(m, st_all, pooled=True):.2f}")
    st_one = {(0, 0): (None, 5, 0.30)}
    print(f" m={m:2d} slots  30% steer in ONE task, slot 0:      stratified {rate(m, st_one):.2f}   pooled {rate(m, st_one, pooled=True):.2f}")
    st_opp = {(t, 0): (None, 5 if t % 2 == 0 else 4, 0.15) for t in range(10)}
    print(f" m={m:2d} slots  opposite-direction 15% steers:      stratified {rate(m, st_opp):.2f}   pooled {rate(m, st_opp, pooled=True):.2f}")
st_all = {(t, 0): (None, 5, 0.10) for t in range(10)}
for u in (0.0, 0.2, 0.4):
    print(f" m= 1 slot   10% steer, undelivered fraction {u:.1f} (ITT, all trials counted): {rate(1, st_all, undeliv=u):.2f}")

# ---------------------------------------------------------------- W2
def paired(N, d, shrink=0.0):
    e = d * (1 - shrink)
    u = rng.random(N) < .6
    v = rng.random(N) < .6 + e
    b = int(np.sum(u & ~v)); c = int(np.sum(v & ~u))
    return b, c

def lower_bound(b, c, N, z=1.645):
    diff = (c - b) / N
    var = ((b + c) / N - diff ** 2) / N
    return diff - z * np.sqrt(max(var, 1e-12))

print("== W2 winner's curse, small real effects, dev->shard shrink; rule: BOTH shards' one-sided 95% lower bound > margin")
def stream(shrink, margin, R=1200, mix=((0.05, 0.05), (0.25, 0.02))):
    prom = 0; small = 0; goodp = 0
    for _ in range(R):
        ds = []
        for _ in range(20):
            x = rng.random(); d = 0.0; acc = 0.0
            for p, e in mix:
                acc += p
                if x < acc: d = e; break
            ds.append(d)
        dev = []
        for d in ds:
            b, c = paired(200, d); dev.append((c - b) / 200)
        k = int(np.argmax(dev))
        ok = True
        for _ in range(2):
            b, c = paired(400, ds[k], shrink)
            if not lower_bound(b, c, 400) > margin: ok = False
        if ok:
            prom += 1
            if ds[k] < 0.03: small += 1
    return prom / R, (small / prom if prom else float('nan'))
for margin in (0.0, 0.02):
    for shrink in (0.0, 0.3, 0.5):
        p, s = stream(shrink, margin)
        print(f" margin {margin:.2f} shrink {shrink:.1f}: promoted per run {p:.3f}; promotions with true effect < 3pp: {s:.2f}")
print(" (mix: 5% of candidates truly +5pp, 25% truly +2pp, rest 0; base 60%; dev N=200, shards N=400)")

# ---------------------------------------------------------------- T1
print("== T1 Tier-1 screens")
def tier1_harm(delta, tasks=30, K=5, reps=600):
    # per-task effect ~ N(delta, 0.05 sd in prob); flag if task-level mean diff one-sided sign-flip p<.10 in the HARM direction
    hit = 0
    for _ in range(reps):
        diffs = []
        for _ in range(tasks):
            p0 = float(np.clip(rng.normal(.6, .2), .05, .95)); p1 = float(np.clip(p0 + delta + rng.normal(0, .05), .01, .99))
            a = np.mean(rng.random(K) < p0); b = np.mean(rng.random(K) < p1)
            diffs.append(b - a)
        d = np.array(diffs); obs = d.sum()
        flips = np.array([np.sum(d * rng.choice([-1, 1], size=tasks)) for _ in range(300)])
        p_harm = (np.sum(flips <= obs + 1e-12) + 1) / 301   # one-sided: obs unusually NEGATIVE
        if p_harm < 0.10: hit += 1
    return hit / reps
for dl in (0.0, -0.05, -0.10, -0.15, -0.25):
    print(f" gross-harm screen (30 tasks x K=5, 300 runs/arm-pair), true effect {dl:+.2f}: flagged {tier1_harm(dl):.2f}")

def binom_lower(x, n, a=0.05):
    lo, hi = 0.0, 1.0
    for _ in range(50):
        mid = (lo + hi) / 2
        tail = sum(comb(n, i) * mid ** i * (1 - mid) ** (n - i) for i in range(x, n + 1))
        if tail < a: lo = mid
        else: hi = mid
    return lo
print(" probe follow-rate screen: pass iff exact 95% LOWER bound of T's follow rate (N=20 probe trials) >= 0.5")
for p in (0.95, 0.85, 0.7, 0.5, 0.3, 0.1):
    ok = np.mean([binom_lower(int(np.sum(rng.random(20) < p)), 20) >= 0.5 for _ in range(600)])
    print(f"   true follow rate {p:.2f}: pass {ok:.2f}")
