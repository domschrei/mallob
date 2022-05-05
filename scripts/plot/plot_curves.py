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
use_twin_axis = []
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
y2label = None
xmin = None
xmax = None
ymin = None
ymax = None
y2min = None
y2max = None
confidence_area = False
legend_right = False
rectangular = False
linewidth = None

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
    elif arg.startswith("-y2label="):
        y2label = arg[9:]
    elif arg.startswith("-xmin="):
        xmin = float(arg[6:])
    elif arg.startswith("-xmax="):
        xmax = float(arg[6:])
    elif arg.startswith("-ymin="):
        ymin = float(arg[6:])
    elif arg.startswith("-ymax="):
        ymax = float(arg[6:])
    elif arg.startswith("-y2min="):
        y2min = float(arg[7:])
    elif arg.startswith("-y2max="):
        y2max = float(arg[7:])
    elif arg.startswith("-lw="):
        linewidth = float(arg[4:])
    elif arg.startswith("-title="):
        heading = arg[7:]
    elif arg.startswith("-y2"):
        use_twin_axis[-1] = True
    elif arg.startswith("-o="):
        outfile = arg[3:]
    elif arg.startswith("-confidence") or arg.startswith("--confidence"):
        confidence_area = True
    elif arg == "-legendright" or arg == "--legendright":
        legend_right = True
    elif arg == "-rect" or arg == "--rect":
        rectangular = True
    elif arg.startswith("-markers="):
        markers = arg[len("-markers="):].split(",")
    elif arg.startswith("-linestyles="):
        linestyles = arg[len("-linestyles="):].split(",")
    elif arg.startswith("-colors="):
        colors = arg[len("-colors="):].split(",")
    elif arg.startswith("-linewidths="):
        linewidth = arg[len("-linewidths="):].split(",")
    else:
        files += [arg]
        use_twin_axis += [False]

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

twin_axis_used = sum([x for x in use_twin_axis if x]) > 0
if twin_axis_used:
    ax = plt.gca()
    ax2 = ax.twinx()

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
    
    if linewidth:
        if type(linewidth) is list:
            kwargs['lw'] = linewidth[i%len(linewidth)]
        else:
            kwargs['lw'] = linewidth
    
    if twin_axis_used:
        if use_twin_axis[i]:
            ax2.plot(d[0], d[1], **kwargs)
        else:
            ax.plot(d[0], d[1], **kwargs)
    else:
        plt.plot(d[0], d[1], **kwargs)
    i += 1

if heading:
    plt.title(heading)
if logx:
    plt.xscale("log")
if logy:
    plt.yscale("log")
if twin_axis_used:
    ax.set_xlabel(xlabel)
    ax.set_ylim(ymin, ymax)
    ax.set_ylabel(ylabel)
    ax2.set_ylim(y2min, y2max)
    ax2.set_ylabel(y2label)
else:
    if xlabel:
        plt.xlabel(xlabel)
    if ylabel:
        plt.ylabel(ylabel)
    plt.ylim(ymin, ymax)
    plt.xlim(xmin, xmax)

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
