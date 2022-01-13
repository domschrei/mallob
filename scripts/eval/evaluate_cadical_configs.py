
import sys
import re

configs = []
instances = []

results = dict()

first = True
for f in sys.argv[1:]:
    match = re.search(r'/([A-Za-z0-9_-]+)\.csv', f)
    if match is None:
        print("ERROR: Could not recognize config file", f)
        exit(0)
    config = match.group(1)
    configs += [config]
    results[config] = []
    for line in open(f, 'r').readlines():
        words = line.rstrip().split(";")
        if first:
            instances += [words[0]]
        results[config] += [float(words[2])]
    first = False

for config in configs:
    par2sum = 0
    num = 0
    numsolved = 0
    for val in results[config]:
        par2sum += val
        num += 1
        if val < 2000:
            numsolved += 1
    par2score = par2sum / num
    print(config,"solved:", numsolved, "/", num, " PAR-2:", par2score)


# Now choose a k-portfolio

portfolio_configs = set()

for k in range(1, len(configs)+1):
    
    best_portfolio_score = 2000
    best_added_config = None

    for c in configs:
        if c in portfolio_configs:
            continue
        psum = 0
        pnum = 0
        pnumsolved = 0
        for instidx in range(len(instances)):
            bestconfig = c
            bestscore = results[c][instidx]
            for other_c in portfolio_configs:
                otherscore = results[other_c][instidx]
                if otherscore < bestscore:
                    bestconfig = other_c
                    bestscore = otherscore
            psum += bestscore
            pnum += 1
            if bestscore < 2000:
                pnumsolved += 1
        pscore = psum / pnum
        if pscore < best_portfolio_score:
            best_portfolio_score = pscore
            best_added_config = c

    if best_added_config is not None:
        portfolio_configs.add(best_added_config)
    
    psolved = 0
    pscore = 0
    for instidx in range(len(instances)):
        val = min([results[c][instidx] for c in portfolio_configs])
        pscore += val
        psolved += 1 if val < 2000 else 0
    pscore /= pnum

    print(k, "portfolio: +", best_added_config, " - solved:", psolved,"/",pnum, " score:", pscore)
