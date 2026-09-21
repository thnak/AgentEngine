import random, math
from math import comb
random.seed(1812)
def sign_p(c, n):
    if n == 0: return 1.0
    k = min(c, n-c); return min(1.0, 2*sum(comb(n,i) for i in range(k+1))/2**n)
def flip_p(d, B=300):
    obs = abs(sum(d)); ge = 0
    for _ in range(B):
        s = sum(x if random.random()<.5 else -x for x in d)
        if abs(s) >= obs-1e-12: ge += 1
    return (ge+1)/(B+1)
R = 300
print("== H1: MEAN-ZERO heterogeneous lesson effect (helps some tasks, hurts others), logit effect ~ N(0,s)")
print("   pooled trial-level sign test  vs  task-level sign-flip on per-task mean differences")
for s in (1.0, 2.0):
    for T,K in ((100,1),(20,5),(10,10)):
        fpp = fpt = 0
        for _ in range(R):
            b=c=0; diffs=[]
            for _ in range(T):
                z = math.log(.6/.4)+random.gauss(0,1.0); e = random.gauss(0,s)
                p0 = 1/(1+math.exp(-z)); p1 = 1/(1+math.exp(-(z+e)))
                u=[random.random()<p0 for _ in range(K)]; v=[random.random()<p1 for _ in range(K)]
                for x,y in zip(u,v): b+=(x and not y); c+=(y and not x)
                diffs.append(sum(v)/K - sum(u)/K)
            if sign_p(c,b+c)<.05: fpp+=1
            if flip_p(diffs)<.05: fpt+=1
        print(f" s={s} T={T:3d} K={K:2d}: pooled FP={fpp/R:.3f}   task-level FP={fpt/R:.3f}")
print("== H2: power for +5pp pooled-independent, larger N")
for N in (800,1200,1600):
    hit=0
    for _ in range(R):
        b=c=0
        for _ in range(N):
            u=random.random()<.6; v=random.random()<.65
            b+=(u and not v); c+=(v and not u)
        if sign_p(c,b+c)<.05 and c>b: hit+=1
    print(f" N={N}: power={hit/R:.2f}")
print("== H3: reruns after invalid/incomplete: null effect, accept first p<.05 across r fresh runs on ONE shard, N=100")
for r in (1,3,5):
    fp=0
    for _ in range(400):
        ok=False
        for _ in range(r):
            b=c=0
            for _ in range(100):
                u=random.random()<.6; v=random.random()<.6
                b+=(u and not v); c+=(v and not u)
            if sign_p(c,b+c)<.05 and c>b: ok=True
        fp+=ok
    print(f" reruns={r}: FP={fp/400:.3f}")
print("== H4: two-shard replication (both one-sided p<.05, N=100 each), null effect")
fp=0
for _ in range(2000):
    ok=True
    for _ in range(2):
        b=c=0
        for _ in range(100):
            u=random.random()<.6; v=random.random()<.6
            b+=(u and not v); c+=(v and not u)
        if not (sign_p(c,b+c)<.05 and c>b): ok=False
    fp+=ok
print(f" FP={fp/2000:.4f}")
print("== H5: safety gate: block iff exact 95% upper bound of T's rate > m=0.02; N=300")
def upper(x,n,a=.05):  # Clopper-Pearson upper via bisection
    lo,hi=0.0,1.0
    for _ in range(50):
        mid=(lo+hi)/2
        cdf=sum(comb(n,i)*mid**i*(1-mid)**(n-i) for i in range(x+1))
        if cdf>a: lo=mid
        else: hi=mid
    return hi
for p in (0.0, 0.005, 0.01, 0.02, 0.04):
    blk=0
    for _ in range(600):
        x=sum(random.random()<p for _ in range(300))
        blk += upper(x,300) > 0.02
    print(f" true rate {p:.3f}: blocked {blk/600:.2f}")
