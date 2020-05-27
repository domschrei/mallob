
import sys
import re
import matplotlib.pyplot as plt
import math

migrations = dict()
reactivations = dict()

for line in sys.stdin:
    
    match = re.search(r'Will receive desc\. of #([0-9]+)', line)
    if match:
        jobid = match.group(1)
        if jobid not in migrations:
            migrations[jobid] = 0
        migrations[jobid] += 1
        print("M " + jobid + "\n")
    
    match = re.search(r'Reactivate #([0-9]+)', line)
    if match:
        jobid = match.group(1)
        if jobid not in reactivations:
            reactivations[jobid] = 0
        reactivations[jobid] += 1
        print("R " + jobid + "\n")


