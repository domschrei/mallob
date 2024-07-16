
import sys
import fileinput
from scipy.stats import gmean

reltime_by_category = dict()
reltime_by_slv = dict()


# Parse

for line in fileinput.input():
    
    words = line.rstrip().split(" ")
    cnf = words[0]
    slv = int(words[1])
    category = words[2]
    abstime = float(words[3])
    reltime = float(words[4])
    
    if category not in reltime_by_category:
        reltime_by_category[category] = []
    reltime_by_category[category] += [reltime]
    
    if slv not in reltime_by_slv:
        reltime_by_slv[slv] = []
    reltime_by_slv[slv] += [reltime]


# Report

for category in reltime_by_category:
    reltimes = sorted(reltime_by_category[category])
    
    out = open(f".profile-histogram-{category}", 'w')
    
    prev = 0
    out.write("0 0\n")
    for reltime in reltimes:
        prev += 1.0 / len(reltimes)
        out.write(f"{reltime} {prev}\n")
