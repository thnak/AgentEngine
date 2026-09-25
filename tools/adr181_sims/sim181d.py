import random
from math import comb
random.seed(1813)
def one_sided(c,n,a=.05):  # exact sign, one-sided, c successes of n discordant
    if n==0: return False
    return sum(comb(n,i) for i in range(c,n+1))/2**n < a
def shard(N,d):
    b=c=0
    for _ in range(N):
        u=random.random()<.6; v=random.random()<.6+d
        b+=(u and not v); c+=(v and not u)
    return one_sided(c,b+c)
R=600
for d in (0.10,0.075,0.05):
    s1=j=0
    for _ in range(R):
        a=shard(400,d); b=shard(400,d); s1+=a; j+=(a and b)
    print(f"+{d:.3f} N=400/shard one-sided a=.05: single={s1/R:.2f} joint(two shards)={j/R:.2f}")
print("null, two shards N=400:", sum(shard(400,0) and shard(400,0) for _ in range(4000))/4000)
