
hitprob = 0.05

def prob_hit_exactly(nhits):
    nhits += 1
    return ((1-hitprob)**(nhits-1)) * hitprob

def prob_hit_atmost(nhits):
    p = 0
    for x in range(nhits+1):
        p += prob_hit_exactly(x)
    return p

probs = []
for nhits in range(300):
    probs += [prob_hit_exactly(nhits)]

#print(prob_hit_atmost(160))

cdf = 0.0
for prob in probs:
    cdf += prob
    print("%.6f" % cdf)
