import random, math
from math import comb, sqrt, erf
random.seed(1811)
def sign_p(c, n):
    if n == 0: return 1.0
    k = min(c, n-c); return min(1.0, 2*sum(comb(n,i) for i in range(k+1))/2**n)
def ztest_p(x1, n1, x2, n2):
    p = (x1+x2)/(n1+n2); se = sqrt(p*(1-p)*(1/n1+1/n2)) or 1e-9
    z = (x1/n1-x2/n2)/se; return 2*(1-0.5*(1+erf(abs(z)/sqrt(2))))
def tp(m, sd):
    z = math.log(m/(1-m)) + random.gauss(0, sd); return 1/(1+math.exp(-z))
R = 400
print("== S1b: NO effect. task_sd=1.5. UNPAIRED (different task draws per arm) vs PAIRED (same tasks)")
for T, K in ((20,5),(10,10)):
    fu = fp = 0
    for _ in range(R):
        pa = [tp(.6,1.5) for _ in range(T)]; pb = [tp(.6,1.5) for _ in range(T)]
        xa = sum(1 if random.random()<p else 0 for p in pa for _ in range(K))
        xb = sum(1 if random.random()<p else 0 for p in pb for _ in range(K))
        if ztest_p(xa,T*K,xb,T*K) < .05: fu += 1
        b=c=0
        for p in pa:
            for _ in range(K):
                u = random.random()<p; v = random.random()<p
                b += (u and not v); c += (v and not u)
        if sign_p(c, b+c) < .05: fp += 1
    print(f" T={T} K={K}: unpaired-z FP={fu/R:.3f}   paired-sign FP={fp/R:.3f}")
print("== S4b: power at delta=+.10 (logit shift applied to the task), N=100 pairs, task_sd 0 vs 1.5")
for sd in (0.0, 1.5):
    for T,K in ((100,1),(20,5)):
        hit=0
        for _ in range(R):
            b=c=0
            for _ in range(T):
                z = math.log(.6/.4)+random.gauss(0,sd)
                p0 = 1/(1+math.exp(-z)); p1 = 1/(1+math.exp(-(z+0.45)))  # ~ +.10 near .6
                for _ in range(K):
                    u = random.random()<p0; v = random.random()<p1
                    b += (u and not v); c += (v and not u)
            if sign_p(c,b+c)<.05 and c>b: hit+=1
        print(f" task_sd={sd} T={T} K={K}: power={hit/R:.2f}")
