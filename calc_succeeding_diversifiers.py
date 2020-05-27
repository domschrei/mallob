
import re
import sys

sd = dict()

for line in sys.stdin:
    if "found result" not in line:
        continue
    match = re.search(r'([0-9\.]+) .*#([0-9]+).*> S([0-9]+) found result', line)
    if match:
        time = float(match.group(1))
        jobid = int(match.group(2))
        sid = int(match.group(3))
        div = sid % 14
        if jobid not in sd:
            # No result for this job so far
            sd[jobid] = [time, [div]]
        else:
            ctime, cdivs = sd[jobid]
            if ctime > time:
                # New result dominates old one
                sd[jobid][0] = time
                sd[jobid][1] = [div]
            if ctime == time and div not in cdivs:
                # New result complements old one
                sd[jobid][1] += [div]
        #print(time,jobid,div)
    
for jobid in sd:
    for div in sd[jobid][1]:
        print(div)
