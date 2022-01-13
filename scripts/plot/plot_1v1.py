#!/usr/bin/env python3
 
import matplotlib.pyplot as plt
import math
import sys

lim = 3600
out = 4000
border_lo = 0.001
border_hi = 5000

msize = 6
pltxsize = 5
pltysize = 5
xmin = None
xmax = None
ymin = None
ymax = None
y2 = None

files = []
runtime_maps = []
labels = []
xlabel = None
ylabel = None
max_id = -1
min_val = lim
heading = ""
outfile = None

for arg in sys.argv[1:]:
    if arg.startswith("-l="):
        labels += [arg[3:]]
    elif arg.startswith("-h="):
        heading = arg[3:]
    elif arg.startswith("-size="):
        pltxsize = float(arg[6:])
        pltysize = float(arg[6:])
    elif arg.startswith("-xsize="):
        pltxsize = float(arg[7:])
    elif arg.startswith("-ysize="):
        pltysize = float(arg[7:])
    elif arg.startswith("-min="):
        border_lo = float(arg[5:])
    elif arg.startswith("-max="):
        border_hi = float(arg[5:])
    elif arg.startswith("-xlabel="):
        xlabel = arg[8:]
    elif arg.startswith("-ylabel="):
        ylabel = arg[8:]
    elif arg.startswith("-T="):
        lim = float(arg[3:])
        out = lim * 1.35
        border_hi = lim * 1.8
        min_val = lim
    elif arg.startswith("-y2="):
        y2 = float(arg[4:])
    elif arg.startswith("-o="):
        outfile = arg[3:]
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
            if val > 0:
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
plt.figure(figsize=(pltxsize,pltysize))
plt.plot([border_lo, border_hi], [border_lo, border_hi], 'black', alpha=0.3, linestyle="--", label="y=x")
if y2:
    plt.plot([1/y2*border_lo, 1/y2*border_hi], [border_lo, border_hi], 'black', alpha=0.3, linestyle="-.", label="y="+str(y2)+"x")
plt.plot([border_lo, lim], [lim, lim], 'blue', alpha=0.3)
plt.plot([lim, lim], [border_lo, lim], 'red', alpha=0.3)
plt.plot(X, Y, 'x', color="black", markersize=msize)

plt.fill_between([border_lo, lim], [lim, lim], [border_hi, border_hi], alpha=0.2, color='blue', label=str(timeouts_y) + " timeouts of LEFT")
plt.fill_between([lim, border_hi], [border_lo, border_lo], [lim, lim], alpha=0.2, color='red', label=str(timeouts_x) + " timeouts of BOTTOM")

if heading:
    plt.title(heading)
if xlabel:
    plt.xlabel(xlabel)
else:    
    plt.xlabel(labels[0] + ' / s')
if ylabel:
    plt.ylabel(ylabel)
else:    
    plt.ylabel(labels[1] + ' / s')
plt.ylim(min_val, border_hi)
plt.xlim(min_val, border_hi)
plt.xscale("log")
plt.yscale("log")
plt.grid(color='#dddddd', linestyle='-', linewidth=1)
plt.legend()
plt.tight_layout()
if outfile:
    plt.savefig(outfile)
else:
    plt.show() 
