import random, math
from math import comb
random.seed(181)

def binom_two_sided(b, n):  # exact sign test on discordant pairs
    if n == 0: return 1.0
    k = min(b, n-b)
    p = sum(comb(n, i) for i in range(0, k+1)) / 2**n
    return min(1.0, 2*p)

def mcnemar_p(base, treat):  # paired binary, exact
    b = sum(1 for x, y in zip(base, treat) if x == 1 and y == 0)
    c = sum(1 for x, y in zip(base, treat) if x == 0 and y == 1)
    return binom_two_sided(c, b + c)

def cluster_boot_p(base_t, treat_t, B=400):
    # per-task mean difference; one-sample bootstrap around 0 by sign flip (task is the unit)
    d = [sum(t)/len(t) - sum(b)/len(b) for b, t in zip(base_t, treat_t)]
    obs = abs(sum(d)/len(d)); n = len(d); ge = 0
    for _ in range(B):
        s = sum(x if random.random() < .5 else -x for x in d)/n
        if abs(s) >= obs - 1e-12: ge += 1
    return (ge+1)/(B+1)

def draw_task_p(base_mean, sd):  # task-level difficulty: logit-normal
    z = math.log(base_mean/(1-base_mean)) + random.gauss(0, sd)
    return 1/(1+math.exp(-z))

print("== S1: cluster effect. NO true lesson effect; T tasks x K trials each arm; alpha .05")
for sd in (0.0, 1.5):
    for T, K in ((100,1),(20,5),(10,10)):
        fp_naive = fp_clu = 0; R = 300
        for _ in range(R):
            bt, tt = [], []
            for _ in range(T):
                p = draw_task_p(.6, sd)
                bt.append([1 if random.random() < p else 0 for _ in range(K)])
                tt.append([1 if random.random() < p else 0 for _ in range(K)])
            flatb = [x for t in bt for x in t]; flatt = [x for t in tt for x in t]
            if mcnemar_p(flatb, flatt) < .05: fp_naive += 1
            if cluster_boot_p(bt, tt, 200) < .05: fp_clu += 1
        print(f" task_sd={sd} T={T:3d} K={K:2d} (N={T*K}) naive-pooled FP={fp_naive/R:.3f}  task-clustered FP={fp_clu/R:.3f}")

print("== S2: selection. NO true effect; pick best of C candidate lessons on the SAME set, test that one")
for C in (1, 5, 10, 20):
    fp_same = fp_split = 0; R = 400; N = 100
    for _ in range(R):
        def arm(): return [1 if random.random() < .6 else 0 for _ in range(N)]
        base = arm()
        cands = [arm() for _ in range(C)]
        # same-set: best candidate by dev gain, tested on same data
        best = max(range(C), key=lambda i: sum(cands[i]))
        if mcnemar_p(base, cands[best]) < .05 and sum(cands[best]) > sum(base): fp_same += 1
        # split: choose on dev, confirm ONCE on fresh held-out
        hb = arm(); hc = arm()
        if mcnemar_p(hb, hc) < .05 and sum(hc) > sum(hb): fp_split += 1
    print(f" C={C:2d}  same-set FP={fp_same/R:.3f}   dev/held-out-once FP={fp_split/R:.3f}")

print("== S3: repeated looks at ONE held-out set (accept if any of L looks with fresh lesson variants p<.05)")
for L in (1, 3, 10):
    fp = 0; R = 400; N = 100
    base = None
    for _ in range(R):
        base = [1 if random.random() < .6 else 0 for _ in range(N)]  # fixed held-out baseline
        hit = False
        for _ in range(L):
            t = [1 if random.random() < .6 else 0 for _ in range(N)]
            if mcnemar_p(base, t) < .05 and sum(t) > sum(base): hit = True
        fp += hit
    print(f" looks={L:2d}  FP={fp/R:.3f}")

print("== S4: power, paired per-item binary, exact sign test, base .6, treat .6+delta (independent items)")
for delta in (.05, .10, .20):
    for N in (50, 100, 200, 400):
        R = 300; hit = 0
        for _ in range(R):
            b = [1 if random.random() < .6 else 0 for _ in range(N)]
            t = [1 if random.random() < .6+delta else 0 for _ in range(N)]
            if mcnemar_p(b, t) < .05 and sum(t) > sum(b): hit += 1
        print(f" delta=+{delta:.2f} N={N:3d} power={hit/R:.2f}")
