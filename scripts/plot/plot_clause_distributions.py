
import sys
import functools
import copy

import matplotlib
from numpy.lib.histograms import histogram, histogram_bin_edges
if False:
    matplotlib.use('pdf')
matplotlib.rcParams['hatch.linewidth'] = 0.5  # previous pdf hatch linewidth
import matplotlib.pyplot as plt
from matplotlib import rc

rc('font', family='serif')
#rc('font', serif=['Times'])
#rc('text', usetex=True)

import matplotlib.animation as animation

histograms_per_solver = dict()

for line in open(sys.argv[1], 'r').readlines():

    line = line.rstrip()
    words = line.split(" ")
    if "clenhist" not in line:
        continue

    try:
        time = float(words[0])
        rank = int(words[1])
        specifier = words[2]

        i = 4
        solverid = -1
        mode = "none"
        while "total:" not in words[i]:
            if words[i][0] == 'S':
                solverid = int(words[i][1:])
            if words[i] == "prod":
                mode = "0_" + words[i]  
            if words[i] == "digd":
                mode = "1_" + words[i]
            i += 1

        if (solverid, mode) not in histograms_per_solver:
            histograms_per_solver[(solverid, mode)] = []
        
        totalnum = int(words[i][6:])
        i += 1
        clslen = 1
        hist = [totalnum]
        while i < len(words):
            hist += [int(words[i])]
            i += 1
            clslen += 1
        while clslen <= 30:
            hist += [0]
            i += 1
            clslen += 1

        histograms_per_solver[(solverid, mode)] += [(time, totalnum, hist)]
        #print(time, solverid, "final" if is_final else "nonfinal", totalnum, hist)

    except:
        print("Skipping:", line)


solverids = [(id, mode) for (id, mode) in histograms_per_solver.keys() if mode == "0_prod"]
def compare(x, y):
    (id1, mode1) = x
    (id2, mode2) = y
    if mode1 != mode2:
        return -1 if mode1 < mode2 else 1
    elif id1 != id2:
        return -1 if id1 < id2 else 1
    else:
        return 0
solverids.sort(key=functools.cmp_to_key(compare))
ididx = 0
histidx = 0

solvercycle = None
if len(sys.argv) > 2:
    solvercycle = sys.argv[2]

def get_solver(sid):
    if sid == -1:
        return "global"
    global solvercycle
    if solvercycle is not None:
        i = sid % len(solvercycle)
        r = int(sid / len(solvercycle))
        solverchar = solvercycle[i]
        if solverchar == 'l':
            out = "lingeling"
        if solverchar == 'L':
            out = "Lingeling"
        if solverchar == 'c':
            out = "cadical"
        if solverchar == 'C':
            out = "Cadical"
        if solverchar == 'g':
            out = "Glucose"
        out += "-" + str(i) + "-" + str(r)
        return out
    return "anonymous"

def get_color(descriptor):
    str = descriptor.lower()
    if "global" in str:
        return "black"
    if "lingeling" in str:
        return "blue"
    if "cadical" in str:
        return "orange"
    if "glucose" in str:
        return "yellow"
    if "anonymous" in str:
        return "gray"
    return None

def animate(j):
    global solverids, ididx, histidx, h, hd, text, fig, ax
    
    (sid, mode) = solverids[ididx]
    while histidx >= len(histograms_per_solver[(sid, mode)]):
        ididx += 1
        histidx = 0
        (sid, mode) = solverids[ididx]

    (time, totalnum, hist) = histograms_per_solver[(sid, mode)][histidx]

    hist = copy.deepcopy(hist)
    if histidx > 0:
        (_1, _2, lasthist) = histograms_per_solver[(sid, mode)][histidx-1]
        for i in range(len(hist)):
            hist[i] = hist[i] - lasthist[i]
    finalhist = [0] + [float(x)/hist[0] if hist[0] > 0 else 0 for x in hist[1:]]

    textstr = "S" + str(sid) + " " + get_solver(sid) + " t=" + str(time) + " produced=" + str(hist[0])

    if h is None:
        h, = ax.plot(finalhist, get_color(textstr))
        text = ax.text(1, 0.95, textstr)
    else:
        h.set_ydata(finalhist)
        h.set_color(get_color(textstr))
        ax.draw_artist(h)
    
    if (sid, "1_digd") in histograms_per_solver and histidx < len(histograms_per_solver[(sid, "1_digd")]):
        (dtime, dtotalnum, dhist) = histograms_per_solver[(sid, "1_digd")][histidx]
        dhist = copy.deepcopy(dhist)
        if histidx > 0:
            (_1, _2, lastdhist) = histograms_per_solver[(sid, "1_digd")][histidx-1]
            for i in range(len(dhist)):
                dhist[i] = dhist[i] - lastdhist[i]
        finaldhist = [0] + [float(x)/dhist[0] if dhist[0] > 0 else 0 for x in dhist[1:]]
        
        if hd is None:
            hd, = ax.plot(finaldhist, get_color(textstr), linestyle="--")
        else:
            hd.set_ydata(finaldhist)
            hd.set_color(get_color(textstr))
            ax.draw_artist(hd)
        
        textstr += " digested=" + str(dhist[0])
    
    print(textstr)
    text.set_text(textstr)
    ax.draw_artist(text)    
    fig.canvas.blit(ax.bbox)

    histidx += 1


fig = plt.figure()
ax = fig.add_subplot(111)
ax.set_xlim(0, 30)
ax.set_ylim(0, 1)
ax.set_xlabel("Clause length")
ax.set_ylabel("Relative occurrence (produced)")

h = None
hd = None
text = None
ani = animation.FuncAnimation(fig, animate, interval=50, repeat=False, cache_frame_data=False)

plt.show()
