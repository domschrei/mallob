#!/usr/bin/env python3
 
import matplotlib.pyplot as plt
import math
import sys
import re

colors = ['#377eb8', '#ff7f00', '#e41a1c', '#f781bf', '#a65628', '#4daf4a', '#984ea3', '#999999', '#dede00', '#377eb8']
markers = ['^', 's', 'o', '+', 'x', 's', '^', '*', 'o']
linestyles = ["-.", ":", "--", "-"]

lim = -1

msize = 10
pltsize = 5

files = []
data = []
labels = []
explicit_xvals = False
do_markers = True
do_lines = True
do_linestyles = True
logx = False
logy = False
xlabel = None
ylabel = None
xmin = None
xmax = None
ymin = None
ymax = None

outfile = None

heading = ""
for arg in sys.argv[1:]:
    if re.search(r'--?log(scale)?-?x', arg):
        logx = True
    elif re.search(r'--?log(scale)?-?y', arg):
        logy = True
    elif re.search(r'--?no-?markers?', arg):
        do_markers = False
    elif re.search(r'--?no-?linestyles?', arg):
        do_linestyles = False
    elif re.search(r'--?no-?lines?', arg):
        do_lines = False
    elif arg.startswith("-xy") or arg.startswith("--xy"):
        explicit_xvals = True
    elif arg.startswith("-l"):
        labels += [arg[2:]]
    elif arg.startswith("-xlabel"):
        xlabel = arg[7:]
    elif arg.startswith("-ylabel"):
        ylabel = arg[7:]
    elif arg.startswith("-xmin"):
        xmin = float(arg[5:])
    elif arg.startswith("-xmax"):
        xmax = float(arg[5:])
    elif arg.startswith("-ymin"):
        ymin = float(arg[5:])
    elif arg.startswith("-ymax"):
        ymax = float(arg[5:])
    elif arg.startswith("-h"):
        heading = arg[2:]
    elif arg.startswith("-o"):
        outfile = arg[2:]
    else:
        files += [arg]


def process_line(line, X, Y, lc):
    
    words = line.rstrip().split(" ")
        
    if not Y:
        if explicit_xvals:
            for i in range(1, len(words)):
                Y += [[]]
        else:
            for i in range(len(words)):
                Y += [[]]
    
    # X value
    if explicit_xvals:
        X += [float(words[0])]
        words = words[1:]
    else:
        X += [lc]
        
    # Y values
    for i in range(len(words)):
        Y[i] += [float(words[i])]


if not files:
    # Read from stdin
    X = []
    Y = []
    lc = 0
    for line in sys.stdin:
        process_line(line, X, Y, lc)
        lc += 1
    print("stdin:",str(len(X)),"vals")
    for vals in Y:
        data += [[X, vals]]
    
else:
    for arg in files:
        X = []
        Y = []
        lc = 0
        for line in open(arg, 'r').readlines():
            process_line(line, X, Y, lc)
            lc += 1
            
        print(arg,":",str(len(X)),"vals")
        for vals in Y:
            data += [[X, vals]]

plt.figure(figsize=(pltsize,pltsize))
i = 0
for d in data:
    print(i)
    
    if i < len(labels):
        l = labels[i]
    else:
        l = None
        
    if do_markers:
        m = markers[i%len(markers)]
    else:
        m = None
    
    if not do_lines:
        lw = 0
    else:
        lw = None
        
    if do_linestyles:
        lst = linestyles[i%len(markers)]
    else:
        lst = None
        
    plt.plot(d[0], d[1], label=l, color=colors[i%len(colors)], marker=m, linestyle=lst, lw=lw)
    i += 1

if heading:
    plt.title(heading)
if xlabel:
    plt.xlabel(xlabel)
if ylabel:
    plt.ylabel(ylabel)
plt.ylim(ymin, ymax)
plt.xlim(xmin, xmax)
if logx:
    plt.xscale("log")
if logy:
    plt.yscale("log")
plt.legend()
plt.tight_layout()
if outfile:
    plt.savefig(outfile)
else:
    plt.show() 
