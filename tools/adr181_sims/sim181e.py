import random, math
from math import comb
random.seed(1814)
def one_sided(c,n,a=.05):
    return n>0 and sum(comb(n,i) for i in range(c,n+1))/2**n < a
def pair_counts(N,d):
    b=c=0
    for _ in range(N):
        u=random.random()<.6; v=random.random()<.6+d
        b+=(u and not v); c+=(v and not u)
    return b,c
def est(N,d):
    b,c=pair_counts(N,d); return (c-b)/N, one_sided(c,b+c)

print("== W1 winner's curse + realistic candidate stream (C=20 cands; 10% truly +0.08, rest 0; dev N=200; shards N=400 x2)")
R=1500; sel_true=[]; sel_dev=[]; prom=0; prom_null=0; prom_good=0; sel_good=0; sel_null=0
for _ in range(R):
    ds=[(0.08 if random.random()<.10 else 0.0) for _ in range(20)]
    devs=[est(200,d)[0] for d in ds]
    k=max(range(20),key=lambda i:devs[i])
    sel_true.append(ds[k]); sel_dev.append(devs[k])
    _,a=est(400,ds[k]); _,b=est(400,ds[k])
    if ds[k]>0: sel_good+=1
    else: sel_null+=1
    if a and b:
        prom+=1
        if ds[k]>0: prom_good+=1
        else: prom_null+=1
print(f" mean dev-estimated effect of the SELECTED candidate: {sum(sel_dev)/R:+.3f}   mean TRUE effect of the selected: {sum(sel_true)/R:+.3f}")
print(f" selected is truly good: {sel_good/R:.2f}  (chance a random candidate is good: 0.10; P(at least one good of 20)=%.2f)" % (1-.9**20))
print(f" promoted overall {prom/R:.3f}; of promotions, truly-null {prom_null/max(prom,1):.2f}; promotions per run that were null: {prom_null/R:.4f}")

print("== P1 paraphrase: closed-domain slot (6 values), B dist fixed; T shifts q mass onto the attacker value (a rare B value); permutation chi-square T vs B")
base=[.40,.30,.15,.10,.03,.02]
def sample(dist,n): 
    r=[random.random() for _ in range(n)]; out=[0]*len(dist); cs=[sum(dist[:i+1]) for i in range(len(dist))]
    for x in r:
        for i,c in enumerate(cs):
            if x<=c: out[i]+=1; break
    return out
def chi(a,b):
    na,nb=sum(a),sum(b); s=0
    for x,y in zip(a,b):
        t=x+y
        if t==0: continue
        ea=t*na/(na+nb); eb=t*nb/(na+nb)
        s+=(x-ea)**2/ea+(y-eb)**2/eb
    return s
def perm_p(a,b,B=200):
    obs=chi(a,b); pool=[]
    for i,x in enumerate(a): pool+= [i]*x
    for i,x in enumerate(b): pool+= [i]*x
    na=sum(a); ge=0
    for _ in range(B):
        random.shuffle(pool)
        aa=[0]*len(a); bb=[0]*len(a)
        for j,v in enumerate(pool):
            (aa if j<na else bb)[v]+=1
        if chi(aa,bb)>=obs: ge+=1
    return (ge+1)/(B+1)
R=200
for n in (150,300):
    for q in (0.0,0.03,0.05,0.10):
        t=[p*(1-q) for p in base]; t[5]+=q
        hit=0
        for _ in range(R):
            a=sample(base,n); b=sample(t,n)
            if perm_p(a,b)<.05: hit+=1
        print(f" N={n}/arm shift q={q:.2f}: detect rate={hit/R:.2f}" + ("  (false-positive rate)" if q==0 else ""))
print("== H7 recheck from measured joint null rate .0015: families needed to reach FP with F variants")
for F in (1,10,100,1000): print(f" F={F}: {1-(1-.0015)**F:.3f}")
