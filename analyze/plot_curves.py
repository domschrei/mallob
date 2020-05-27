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
pltxsize = 5
pltysize = 5

files = []
data = []
labels = []
explicit_xvals = False
colorvals = False
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
        do_linestyles = False
    elif arg.startswith("-xyc") or arg.startswith("--xyc"):
        explicit_xvals = True
        colorvals = True
    elif arg.startswith("-xy") or arg.startswith("--xy"):
        explicit_xvals = True
    elif arg.startswith("-size="):
        pltxsize = float(arg[6:])
        pltysize = float(arg[6:])
    elif arg.startswith("-xsize="):
        pltxsize = float(arg[7:])
    elif arg.startswith("-ysize="):
        pltysize = float(arg[7:])
    elif arg.startswith("-l="):
        labels += [arg[3:]]
    elif arg.startswith("-xlabel="):
        xlabel = arg[8:]
    elif arg.startswith("-ylabel="):
        ylabel = arg[8:]
    elif arg.startswith("-xmin="):
        xmin = float(arg[6:])
    elif arg.startswith("-xmax="):
        xmax = float(arg[6:])
    elif arg.startswith("-ymin="):
        ymin = float(arg[6:])
    elif arg.startswith("-ymax="):
        ymax = float(arg[6:])
    elif arg.startswith("-title="):
        heading = arg[7:]
    elif arg.startswith("-o="):
        outfile = arg[3:]
    else:
        files += [arg]


def process_line(line, X, Y, C, lc):
    
    words = line.rstrip().split(" ")
    
    num_ys = len(words)
    if colorvals:
        num_ys -= 1
    
    if not Y:
        if explicit_xvals:
            for i in range(1, num_ys):
                Y += [[]]
        else:
            for i in range(num_ys):
                Y += [[]]
        
    # X value
    if explicit_xvals:
        X += [float(words[0])]
        words = words[1:]
        if colorvals:
            C += [int(words[-1])]
            words = words[0:-1]
    else:
        X += [lc]
        
    # Y values
    for i in range(len(words)):
        Y[i] += [float(words[i])]


if not files:
    # Read from stdin
    X = []
    Y = []
    C = []
    lc = 0
    for line in sys.stdin:
        process_line(line, X, Y, C, lc)
        lc += 1
        
    print("stdin:",str(len(X)),"vals")
    for vals in Y:
        data += [[X, vals, C]]
    
else:
    for arg in files:
        X = []
        Y = []
        C = []
        lc = 0
        for line in open(arg, 'r').readlines():
            process_line(line, X, Y, C, lc)
            lc += 1
            
        print(arg,":",str(len(X)),"vals")
        for vals in Y:
            data += [[X, vals, C]]

plt.figure(figsize=(pltxsize,pltysize))
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
        lst = linestyles[i%len(linestyles)]
    else:
        lst = None
    
    if colorvals:
        clr = [colors[x%len(colors)] for x in d[2]]
        #if do_markers:
        #    m = [markers[x%len(markers)] for x in d[2]]
    else:
        clr = colors[i%len(colors)]
    
    if lst:
        plt.scatter(d[0], d[1], label=l, color=clr, marker=m, linestyle=lst, lw=lw)
    else:
        plt.scatter(d[0], d[1], label=l, color=clr, marker=m, lw=lw)
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
