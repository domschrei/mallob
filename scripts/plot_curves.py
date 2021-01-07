#!/usr/bin/env python3
 
import math
import sys
import re

colors = ['#377eb8', '#ff7f00', '#e41a1c', '#f781bf', '#a65628', '#4daf4a', '#984ea3', '#999999', '#dede00', '#377eb8']
markers = ['^', 's', 'o', '+', 'x', '*']
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
do_legend = True
do_xgrid = False
do_ygrid = False
logx = False
logy = False
xlabel = None
ylabel = None
xmin = None
xmax = None
ymin = None
ymax = None
confidence_area = False
legend_right = False
rectangular = True

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
    elif re.search(r'--?no-?legend', arg):
        do_legend = False
    elif re.search(r'--?xgrid', arg):
        do_xgrid = True
    elif re.search(r'--?ygrid', arg):
        do_ygrid = True
    elif re.search(r'--?grid', arg):
        do_xgrid = True
        do_ygrid = True
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
    elif arg.startswith("-confidence") or arg.startswith("--confidence"):
        confidence_area = True
    elif arg == "-legendright" or arg == "--legendright":
        legend_right = True
    else:
        files += [arg]

import matplotlib
if outfile:
    matplotlib.use('pdf')
matplotlib.rcParams['hatch.linewidth'] = 0.5  # previous pdf hatch linewidth
import matplotlib.pyplot as plt
from matplotlib import rc
rc('font', family='serif')
#rc('font', serif=['Times'])
rc('text', usetex=True)

def process_line(line, X, Y, C, lc):
    
    words = line.rstrip().split(" ")
    
    # check validity
    for word in words:
        try:
            x = float(word)
        except:
            return
    
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
    if rectangular and len(X) > 0:
        x = float(words[0])-0.0001
        X += [x]
    if explicit_xvals:
        x = float(words[0])
        X += [x]
        words = words[1:]
        if colorvals:
            C += [int(words[-1])]
            words = words[0:-1]
    else:
        X += [lc]
        
    # Y values
    for i in range(len(words)):
        y = float(words[i])
        if rectangular and len(Y[i]) > 0:
            Y[i] += [Y[i][-1]]
        Y[i] += [y]

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

plt.axes(axisbelow=True)
if do_xgrid and do_ygrid:
    plt.grid(axis='both')
elif do_xgrid:
    plt.grid(axis='x')
elif do_ygrid:
    plt.grid(axis='y')

if confidence_area:
    plt.fill_between(data[1][0], data[1][1], data[2][1], color='#ccccff')

i = 0
for d in data:
    print(i)
    
    kwargs = dict()
    
    if i < len(labels):
        kwargs['label'] = labels[i]
    
    if colorvals:
        kwargs['color'] = [colors[x%len(colors)] for x in d[2]]
    else:
        kwargs['color'] = colors[i%len(colors)]
        
    if do_markers:
        kwargs['marker'] = markers[i%len(markers)]
        if kwargs['marker'] in ['+', 'x']:
            kwargs['markerfacecolor'] = kwargs['color']
        else:
            kwargs['markerfacecolor'] = 'none'
    
    if not do_lines:
        kwargs['lw'] = 0
    else:
        kwargs['lw'] = 1
        
    if do_linestyles:
        kwargs['linestyle'] = linestyles[i%len(linestyles)]
    
    plt.plot(d[0], d[1], **kwargs)
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
if do_legend:
    if legend_right:
        plt.legend(bbox_to_anchor=(1.05, 0.5), loc='center left', edgecolor="black")
    else:
        plt.legend()
plt.tight_layout()
if outfile:
    plt.savefig(outfile, dpi=300)
else:
    plt.show() 
