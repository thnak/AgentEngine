"""ADR-195 round-4 fixes. Needs numpy. Fixed seeds.

G1  containment gate: N pre-registered as DELIVERED trials, harness runs extra pairs until N delivered
    T trials collected (capped); shows the exact zero-event bound is restored regardless of delivery rate,
    at a stated extra-run cost.
G2  divergence detector: ITT (no delivery filter) vs delivered-only, under a mutant where delivery is
    correlated with the slot value (breaks exchangeability for the delivered-only filter). Also power at
    the real Tier-1 N=150/arm.
G3  gross-harm screen: a per-task tripwire (any task >= 3-of-5 failures beyond baseline) alongside the
    existing sum-based sign-flip test, for harm concentrated in a few tasks.
G4  probe follow-rate screen: "all of k must pass" vs "any of k passes" (round-4 finding: "any" inflates
    the false-pass rate; fix is "all", k=1 default).
G5  dev->shard shift (Tier 2): reproduced independently from the round-4 reviewer's design.
"""
import numpy as np
from math import comb

rng = np.random.default_rng(1816)

# ---------------------------------------------------------------- G1
def binom_upper(x, n, a=0.05):
    lo, hi = 0.0, 1.0
    for _ in range(60):
        mid = (lo + hi) / 2
        tail = sum(comb(n, i) * mid ** i * (1 - mid) ** (n - i) for i in range(0, x + 1))
        if tail > a: lo = mid
        else: hi = mid
    return hi

print("== G1 containment gate: N pre-registered as DELIVERED trials (fix for the N=150-total knife-edge)")
print(" zero-event 95% upper bound vs DELIVERED N:")
for n in (135, 148, 149, 150, 200, 300):
    print(f"   N_delivered={n:3d}: upper bound {binom_upper(0, n):.4f}")
print(" expected TOTAL trials needed to reach 150 delivered, by delivery rate (geometric, capped at 4x):")
for delivery in (1.0, 0.9, 0.75, 0.5, 0.3):
    need = 150
    expected_total = need / delivery
    capped = min(expected_total, 150 * 4)
    print(f"   delivery {delivery:.2f}: expected total trials {expected_total:.0f}"
          f"{' (capped at 600 -> blocked: insufficient_n, delivery too low to reach N)' if expected_total > 600 else ''}")

# ---------------------------------------------------------------- G2
def chi_stat_pooled(ct, cb):
    tot = ct + cb
    nt = ct.sum(); nb = cb.sum(); n = nt + nb
    with np.errstate(divide='ignore', invalid='ignore'):
        et = tot * nt / n; eb = tot * nb / n
        s = np.where(et > 0, (ct - et) ** 2 / et, 0.0) + np.where(eb > 0, (cb - eb) ** 2 / eb, 0.0)
    return np.nansum(s)

def run_g2(N, delivery_corr_with_rare, q=0.0, B=300, reps=300, delivered_only=False):
    base = np.array([.40, .30, .15, .10, .03, .02])
    V = len(base)
    flags = 0
    for _ in range(reps):
        t = rng.choice(V, p=base, size=N)
        b = rng.choice(V, p=base, size=N)
        if q > 0:
            hit = rng.random(N) < q
            t[hit] = 5
        if delivery_corr_with_rare:
            # delivery probability is higher when the trial's own draw is a rare value -- a latent
            # confound between "what this trial would show" and "whether it got delivered"
            p_delivered = np.where(np.isin(t, [4, 5]), 0.95, 0.5)
            delivered = rng.random(N) < p_delivered
        else:
            delivered = np.ones(N, dtype=bool)
        if delivered_only:
            t_used = t[delivered]
        else:
            t_used = t  # ITT: undelivered trials still counted at whatever the (null) request produced
        if len(t_used) < 5:
            continue
        oh_t = np.eye(V)[t_used].sum(axis=0)
        oh_b = np.eye(V)[b].sum(axis=0)
        obs = chi_stat_pooled(oh_t, oh_b)
        pooled = np.concatenate([t_used, b])
        nT = len(t_used)
        ge = 0
        for _ in range(B):
            perm = rng.permutation(pooled)
            ct2 = np.eye(V)[perm[:nT]].sum(axis=0); cb2 = np.eye(V)[perm[nT:]].sum(axis=0)
            if chi_stat_pooled(ct2, cb2) >= obs - 1e-9: ge += 1
        p = (ge + 1) / (B + 1)
        if p < 0.05: flags += 1
    return flags / reps

print("== G2 divergence detector: ITT vs delivered-only, under a delivery~value confound mutant (no real steer)")
for corr in (False, True):
    fi = run_g2(150, corr, q=0.0, delivered_only=False, reps=250)
    fd = run_g2(150, corr, q=0.0, delivered_only=True, reps=250)
    print(f" delivery correlated with rare value = {corr}: false-flag  ITT {fi:.3f}   delivered-only {fd:.3f}")
print(" power at the real Tier-1 N=150/arm (ITT, no confound), 1 declared slot:")
for q in (0.10, 0.05, 0.0):
    pw = run_g2(150, False, q=q, delivered_only=False, reps=250)
    print(f"   steer {q:.0%}: flagged {pw:.2f}")

# ---------------------------------------------------------------- G3
def sum_perm_pvalue(diffs, reps_perm=300):
    # existing sum-based sign-flip test (task-level diffs are already fixed magnitudes; sign-flip is
    # valid here because the null hypothesis being tested is "no systematic direction", and the sum is
    # blind to which task the magnitude sits in)
    d = np.array(diffs); obs = d.sum()
    flips = np.array([np.sum(d * rng.choice([-1, 1], size=len(d))) for _ in range(reps_perm)])
    return (np.sum(flips <= obs + 1e-12) + 1) / (reps_perm + 1)

# A fixed-threshold "any task diff >= 3-of-5" tripwire was tried first and had a 67% false-flag rate at
# nominal alpha=0.10: 30 independent per-task comparisons is a multiple-testing problem an uncorrected
# threshold does not control. A sign-flip permutation on a MIN statistic was tried second and had ~0%
# power: sign-flipping preserves each task's magnitude |d_i| exactly, so the one task that is genuinely
# extreme is exactly as extreme half the time under the null permutation too -- sign-flip cannot
# distinguish "one huge task" from "one task that happened to flip negative". The statistic needs a
# permutation null built from the underlying TRIALS, not from the already-collapsed task diffs: for each
# task, holding its total successes s_i = a_i+b_i fixed, redraw how many of those s_i successes land in
# the B group by an unordered draw from 2K trials (hypergeometric) -- the randomization a real
# trial-level relabelling would produce -- and take the extreme task diff over all 30 simultaneously.
def min_task_perm_pvalue(a_arr, b_arr, K, reps_perm=300):
    a_arr = np.asarray(a_arr); b_arr = np.asarray(b_arr)
    obs = float(np.min(b_arr - a_arr))
    s = a_arr + b_arr
    mins = np.empty(reps_perm)
    for r in range(reps_perm):
        a2 = rng.hypergeometric(s, 2 * K - s, K)
        b2 = s - a2
        mins[r] = np.min(b2 - a2)
    return (np.sum(mins <= obs + 1e-9) + 1) / (reps_perm + 1)

def concentrated_harm_trial(tasks=30, K=5, n_bad=3, bad_effect=-0.5, reps=600):
    sum_flag = 0; min_flag = 0; either = 0
    for _ in range(reps):
        p0 = np.clip(rng.normal(.6, .2, tasks), .05, .95)
        p1 = p0.copy()
        bad = rng.choice(tasks, size=n_bad, replace=False)
        p1[bad] = np.clip(p1[bad] + bad_effect, .01, .99)
        a = rng.binomial(K, p0); b = rng.binomial(K, p1)
        diffs = (b - a) / K
        sf = sum_perm_pvalue(diffs) < 0.10
        mf = min_task_perm_pvalue(a, b, K) < 0.10
        if sf: sum_flag += 1
        if mf: min_flag += 1
        if sf or mf: either += 1
    return sum_flag / reps, min_flag / reps, either / reps

print("== G3 gross-harm screen: sum statistic vs a trial-level-permutation min-task (concentration) statistic")
def null_rate(tasks=30, K=5, reps=600):
    sum_flag = 0; min_flag = 0
    for _ in range(reps):
        p0 = np.clip(rng.normal(.6, .2, tasks), .05, .95)
        a = rng.binomial(K, p0); b = rng.binomial(K, p0)
        diffs = (b - a) / K
        if sum_perm_pvalue(diffs) < 0.10: sum_flag += 1
        if min_task_perm_pvalue(a, b, K) < 0.10: min_flag += 1
    return sum_flag / reps, min_flag / reps
sn, mn = null_rate()
print(f" no effect (false flag): sum-stat {sn:.3f}   min-task-stat {mn:.3f}")
for n_bad in (1, 3, 5):
    s, m, e = concentrated_harm_trial(n_bad=n_bad)
    print(f" {n_bad} of 30 tasks fully broken (-50pp each): sum-stat {s:.2f}   min-task-stat {m:.2f}   either {e:.2f}")
# uniform (non-concentrated) harm, min-task-stat vs sum-stat, sanity check against T1's existing numbers
def uniform_harm_trial(delta, tasks=30, K=5, reps=600):
    sum_flag = 0; min_flag = 0
    for _ in range(reps):
        p0 = np.clip(rng.normal(.6, .2, tasks), .05, .95)
        p1 = np.clip(p0 + delta + rng.normal(0, .05, tasks), .01, .99)
        a = rng.binomial(K, p0); b = rng.binomial(K, p1)
        diffs = (b - a) / K
        if sum_perm_pvalue(diffs) < 0.10: sum_flag += 1
        if min_task_perm_pvalue(a, b, K) < 0.10: min_flag += 1
    return sum_flag / reps, min_flag / reps
for dl in (-0.05, -0.10, -0.15):
    s, m = uniform_harm_trial(dl)
    print(f" uniform harm {dl:+.2f} (not concentrated): sum-stat {s:.2f}   min-task-stat {m:.2f}  (sum-stat should match T1's earlier numbers)")

# ---------------------------------------------------------------- G4
def probe_screen(true_rate, k, mode, reps=1500):
    passed = 0
    for _ in range(reps):
        results = []
        for _ in range(k):
            x = int(np.sum(rng.random(20) < true_rate))
            lb = binom_lower(x, 20)
            results.append(lb >= 0.5)
        if mode == 'any': ok = any(results)
        elif mode == 'all': ok = all(results)
        else: ok = results[0]
        if ok: passed += 1
    return passed / reps

def binom_lower(x, n, a=0.05):
    lo, hi = 0.0, 1.0
    for _ in range(50):
        mid = (lo + hi) / 2
        tail = sum(comb(n, i) * mid ** i * (1 - mid) ** (n - i) for i in range(x, n + 1))
        if tail < a: lo = mid
        else: hi = mid
    return lo

print("== G4 probe follow-rate screen: any-of-k (round-4 finding: inflates) vs all-of-k vs k=1 (the fix)")
for true_rate in (0.7, 0.5, 0.3):
    p1 = probe_screen(true_rate, 1, 'one')
    p_any3 = probe_screen(true_rate, 3, 'any')
    p_all3 = probe_screen(true_rate, 3, 'all')
    print(f" true follow rate {true_rate:.2f}: k=1 {p1:.2f}   any-of-3 {p_any3:.2f}   all-of-3 {p_all3:.2f}")

# ---------------------------------------------------------------- G5
def dev_shard_shift(s, corr=0.5, reps=1500, margin=0.02, se_shard=0.027):
    false_promo = 0
    for _ in range(reps):
        # 20 candidates, all truly zero deployment effect; each has a task-mix interaction term
        # correlated between dev and shard by `corr`
        dev_term = rng.normal(0, s, 20)
        noise = rng.normal(0, s, 20)
        shard_term = corr * dev_term + np.sqrt(max(1 - corr ** 2, 0)) * noise
        dev_est = dev_term + rng.normal(0, 0.03, 20)  # dev sampling noise, SE~0.03 (N=200)
        k = int(np.argmax(dev_est))
        b1 = shard_term[k] + rng.normal(0, se_shard)
        b2 = shard_term[k] + rng.normal(0, se_shard)
        lb1 = b1 - 1.645 * se_shard
        lb2 = b2 - 1.645 * se_shard
        if lb1 > margin and lb2 > margin:
            false_promo += 1
    return false_promo / reps

print("== G5 dev->shard shift (Tier 2), reproduced independently: 20 candidates all truly zero, task-mix interaction sd=s")
for s in (0.0, 0.02, 0.03, 0.05):
    r = dev_shard_shift(s)
    print(f" s={s:.2f}, dev-shard corr 0.5: false promotions per batch {r:.3f}")

# ---------------------------------------------------------------- G6
def h5_block_rate(p, n, margin=0.02, reps=500):
    # cache: only distinct x values need a fresh (slow, arbitrary-precision) binom_upper call
    x = rng.binomial(n, p, size=reps)
    cache = {}
    blocked = 0
    for xi in x:
        xi = int(xi)
        if xi not in cache:
            cache[xi] = binom_upper(xi, n) > margin
        if cache[xi]: blocked += 1
    return blocked / reps

print("== G6 H5 safety gate re-measured at the real Tier-1 default N=150 (H5's original evidence was at N=300)")
for n in (150, 300):
    row = " / ".join(f"{p:.3f}: {h5_block_rate(p, n):.2f}" for p in (0.0, 0.005, 0.01, 0.02, 0.04))
    print(f" N={n}: {row}")
