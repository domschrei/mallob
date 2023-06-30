#!/usr/bin/env python3

import math
import sys

markers = ['^', 'v', 'x']
colors = ['#377eb8', '#ff7f00', '#e41a1c', '#f781bf', '#a65628', '#4daf4a', '#984ea3', '#999999', '#dede00', '#377eb8']

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
min_val = lim
heading = ""
outfile = None
logscale = False
domainlabels = None
domainmarkers = None
domaincolors = None
tickslist = None

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
        #out = lim * 1.35
        #border_hi = lim * 1.8
        min_val = lim
    elif arg.startswith("-y2="):
        y2 = float(arg[4:])
    elif arg.startswith("-o="):
        outfile = arg[3:]
    elif arg.startswith("-logscale"):
        logscale = True
    elif arg.startswith("-domainlabels="):
        domainlabels = arg[len("-domainlabels="):].split(",")
    elif arg.startswith("-domainmarkers="):
        domainmarkers = arg[len("-domainmarkers="):].split(",")
    elif arg.startswith("-domaincolors="):
        domaincolors = arg[len("-domaincolors="):].split(",")
    elif arg.startswith("-ticks="):
        tickslist = arg[len("-ticks="):].split(",")
    elif arg.startswith("-markersize="):
        msize = float(arg[len("-markersize="):])
    else:
        files += [arg]

import matplotlib
if outfile:
    matplotlib.use('pdf')
matplotlib.rcParams['hatch.linewidth'] = 0.5  # previous pdf hatch linewidth
import matplotlib.pyplot as plt
from matplotlib import rc

sansfont = False
timesfont = False

rc('text', usetex=True)
if sansfont:
    matplotlib.rcParams['text.latex.preamble'] = [r'\usepackage[cm]{sfmath}']
    matplotlib.rcParams['font.family'] = 'sans-serif'
    matplotlib.rcParams['font.sans-serif'] = 'cm'
    #\renewcommand\familydefault{\sfdefault} 
else:
    rc('font', family='serif')
    if timesfont:
        rc('font', serif=['Times'])

out = math.exp((math.log(lim) + math.log(border_hi)) / 2)

runtime_map_pairs_by_domain = dict()

for arg in files:
    domains_seen = set()
    for line in open(arg, 'r').readlines():
        words = line.rstrip().split(" ")
        id = words[0]
        dom = words[1]
        val = float(words[2])
        if dom not in runtime_map_pairs_by_domain:
            runtime_map_pairs_by_domain[dom] = []
        if dom not in domains_seen:
            runtime_map_pairs_by_domain[dom] += [dict()]
            domains_seen.add(dom)
        id_runtime_map = runtime_map_pairs_by_domain[dom][-1]
        if val <= lim:
            id_runtime_map[id] = val
            if val > 0:
                min_val = min(min_val, val)

for dom in runtime_map_pairs_by_domain:
    if len(runtime_map_pairs_by_domain[dom]) != 2:
        print("Need exactly two runtime files for each domain!")
        exit(1)

margin = lim - out

fig, ax = plt.subplots(1, 1, figsize=(pltxsize, pltysize))
ax.set_box_aspect(1)

if tickslist:
    ax.set_xticklabels(tickslist)
    ax.set_xticks([float(x) for x in tickslist])
    ax.set_yticklabels(tickslist)
    ax.set_yticks([float(x) for x in tickslist])
    plt.minorticks_off()

plt.grid(color='#dddddd', linestyle='-', linewidth=1)

plt.plot([border_lo, lim], [border_lo, lim], 'black', alpha=0.3, linestyle="--", label="y=x")
#plt.plot([border_lo, border_hi], [10*border_lo, 10*border_hi], 'gray', alpha=0.3, linestyle="--", label="y=10x")
if y2:
    plt.plot([1/y2*border_lo, 1/y2*border_hi], [border_lo, border_hi], 'black', alpha=0.3, linestyle="-.", label="y="+str(y2)+"x")

plt.plot([border_lo, lim], [lim, lim], 'black', alpha=1)
plt.plot([lim, lim], [border_lo, lim], 'black', alpha=1)
plt.fill_between([border_lo, lim], [lim, lim], [border_hi, border_hi], alpha=0.3, color='gray', zorder=0) #color='blue', label=str(timeouts_y) + " timeouts of LEFT")
plt.fill_between([lim, border_hi], [border_lo, border_lo], [lim, lim], alpha=0.3, color='gray', zorder=0) #, label=str(timeouts_x) + " timeouts of BOTTOM")

timeouts_x = 0
timeouts_y = 0
marker_idx = 0
color_idx = 0
label_idx = 0


for dom in runtime_map_pairs_by_domain:
    runtime_maps = runtime_map_pairs_by_domain[dom]
    
    X = []
    Y = []
    keys = [i for i in runtime_maps[0]] + [i for i in runtime_maps[1]]
    for i in keys:
        
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

    plt.plot(X, Y, domainmarkers[marker_idx] if domainmarkers else markers[marker_idx],
             color=domaincolors[color_idx] if domaincolors else colors[color_idx], 
             label=domainlabels[label_idx] if domainlabels else dom,
             alpha=1, markersize=msize, markeredgecolor='None')
    marker_idx = (marker_idx+1) % len(markers)
    color_idx = (color_idx+1) % len(markers)
    label_idx += 1

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
if logscale:
    plt.xscale("log")
    plt.yscale("log")

plt.legend()

plt.tight_layout()
if outfile:
    plt.savefig(outfile)
else:
    plt.show() 

