
import sys
import re

loads = []

for line in open(sys.argv[1], 'r').readlines():
    if "LOAD" not in line:
        continue

    match = re.match(r'([0-9\.]+) ([0-9]+) LOAD ([01]) (.*)', line)
    while match is not None:
        time = float(match.group(1))
        rank = int(match.group(2))
        load = int(match.group(3))

        while rank >= len(loads):
            loads += [[]]
        rankloads = loads[rank]
        
        if len(rankloads) > 0:
            (lasttime, lastload) = rankloads[-1]
            if lastload == load:
                print("WARN: inconsistency in load report detected")
                print(lasttime, lastload)
                print(line)

            elif load == 1 and time-lasttime >= 3:
                print("Rank", rank, "was idle for", time-lasttime, "s ( beginning from", lasttime, ")")
                #print(line)

        rankloads += [(time, load)]
        
        line = match.group(4)
        match = re.match(r'([0-9\.]+) ([0-9]+) LOAD ([01]) (.*)', line)


#querytime = 457

#for rank in range(len(loads)):


