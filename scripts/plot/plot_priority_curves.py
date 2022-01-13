
import matplotlib.pyplot as plt

values = []

for line in open("_priority_curves", "r").readlines():
    words = line.rstrip().split(" ")
    
    lb = float(words[0])
    ub = float(words[2])
    demand = float(words[3])
    values += [[lb, ub, demand]]
    
    x1 = float(words[0])
    y1 = float(words[1])
    x2 = float(words[2])
    y2 = float(words[3])
    plt.plot([x1, x2], [y1, y2])
    
"""
sumx = []
sumy = []
x = 0.0001
while x < 1000:
    
    total = 0
    for [lb, ub, demand] in values:
        if ub - lb == 0:
            total += 1
        else:
            ratio = (x - lb) / (ub - lb)
            total += max(1, min(demand, 1 + ratio * (demand-1)))
    
    sumx += [x]
    sumy += [total]
    x *= 1.01
    
plt.plot(sumx, sumy)
"""

plt.show()
