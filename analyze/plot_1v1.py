#!/usr/bin/env python3
 
import matplotlib.pyplot as plt
import math
import sys

lim = 3600
out = 3600
border_lo = 0.001
border_hi = 5000

msize = 10
pltsize = 5

files = []
runtime_maps = []
labels = []
max_id = -1
min_val = lim
heading = ""
for arg in sys.argv[1:]:
    if arg.startswith("-l"):
        labels += [arg[2:]]
    elif arg.startswith("-h"):
        heading = arg[2:]
    elif arg.startswith("-T"):
        lim = int(arg[2:])
        out = lim
        border_hi = lim * 1.6
        min_val = lim
    else:
        files += [arg]

for arg in files:
    id_runtime_map = dict()
    for line in open(arg, 'r').readlines():
        words = line.rstrip().split(" ")
        id = int(words[0])
        val = float(words[2])
        if val <= lim:
            id_runtime_map[id] = val
            max_id = max(max_id, id)
            min_val = min(min_val, val)
    runtime_maps += [id_runtime_map]

if len(runtime_maps) != 2:
    print("Need exactly two runtime files!")
    exit(1)

X = []
Y = []
timeouts_x = 0
timeouts_y = 0
for i in range(max_id+1):
    
    if i not in runtime_maps[0] and i not in runtime_maps[1]:
        continue
    
    elif i not in runtime_maps[0]:
        Y += [runtime_maps[1][i]]
        X += [out]
        timeouts_x += 1
        print(str(i) + " : X timeout , Y " + str(runtime_maps[1][i]))
    elif i not in runtime_maps[1]:
        X += [runtime_maps[0][i]]
        Y += [out]
        timeouts_y += 1
        print(str(i) + " : X " + str(runtime_maps[0][i]) + ", Y timeout")
    else:
        X += [runtime_maps[0][i]]
        Y += [runtime_maps[1][i]]
        print(str(i) + " : X " + str(runtime_maps[0][i]) + ", Y " + str(runtime_maps[1][i]))

margin = lim - out
plt.figure(figsize=(pltsize,pltsize))
plt.plot([border_lo, border_hi], [border_lo, border_hi], 'black', alpha=0.3, linestyle="--", label="x=y")
plt.plot([border_lo, lim], [lim, lim], 'blue', alpha=0.3, label=str(timeouts_y) + " timeouts of LEFT")
plt.plot([lim, lim], [border_lo, lim], 'red', alpha=0.3, label=str(timeouts_x) + " timeouts of BOTTOM")
plt.plot(X, Y, 'x', color="black", markersize=msize)

if heading:
    plt.title(heading)
plt.xlabel(labels[0] + ' / s')
plt.ylabel(labels[1] + ' / s')
plt.ylim(min_val, border_hi)
plt.xlim(min_val, border_hi)
plt.xscale("log")
plt.yscale("log")
plt.legend()
plt.tight_layout()
#plt.savefig(iA + '_' + iB + '.pdf')
plt.show() 
