"""
Takes one or multiple log files of a single Mallob run with displayed warmup messages.
Computes small shifts in the individual workers' log timestamps
such that all warmup messages are temporally coherent.
Writes the adapted file of each given file to "<arg>_harmonized".
"""

import sys
from ortools.linear_solver import pywraplp

msgPairs = dict()
OFFSET_ABS_VAR = 10000

for arg in sys.argv[1:]:
    for line in open(arg, "r").readlines():
        line = line.rstrip()
        if "warmup msg" not in line:
            continue
        
        # 0.655 2 Received warmup msg <= [4]
        #print(line)
        words = line.split(" ")
        try:
            time = float(words[0])
            rank = int(words[1])
            otherRankStr = words[6]
            otherRank = int(otherRankStr[1:len(otherRankStr)-1])
            received = words[2] == "Received"

            recvRank = rank if received else otherRank
            sendRank = otherRank if received else rank

            if (sendRank, recvRank) not in msgPairs:
                # Key: (sending rank, receiving rank)
                # Value: (sending time, receiving time)
                msgPairs[(sendRank, recvRank)] = [None, None]
            msgPairs[(sendRank, recvRank)][1 if received else 0] = time
        except ValueError:
            print("WARN: Did not manage to parse", line)
            continue

solver = pywraplp.Solver.CreateSolver('GLOP')
vars = dict()

for (sendRank, recvRank) in msgPairs:
    [sendTime, recvTime] = msgPairs[(sendRank, recvRank)]
    if sendTime is None or recvTime is None:
        print("WARN: Skipping incomplete pair", [sendRank, recvRank])
        continue
    for rank in [sendRank, recvRank]:
        if rank not in vars:
            vars[rank] = solver.NumVar(-60, 60, "o"+str(rank))
            vars[rank+OFFSET_ABS_VAR] = solver.NumVar(0, 60, "a"+str(rank)) 
            solver.Add(vars[rank+OFFSET_ABS_VAR] >= vars[rank])
            solver.Add(vars[rank+OFFSET_ABS_VAR] >= -vars[rank])

    # 0.655+o2 2 Sending warmup msg <= [4]
    # 0.631+o4 4 Received warmup msg <= [2]
    # o4-o2 >= 0.655-0.631
    epsilon = 0.0005
    solver.Add(vars[recvRank] - vars[sendRank] + epsilon >= (sendTime - recvTime))

print('Number of variables =', solver.NumVariables())
print('Number of constraints =', solver.NumConstraints())

# Objective function: sum of absolute value of each offset
objfunc = None
for rank in vars:
    if rank < OFFSET_ABS_VAR:
        continue
    var = vars[rank]
    if objfunc is None:
        objfunc = var
    else:
        objfunc = objfunc + var
solver.Minimize(objfunc)

status = solver.Solve()

if status != pywraplp.Solver.OPTIMAL:
    print('The problem does not have an optimal solution.')
    if status != pywraplp.Solver.FEASIBLE:
        print('The problem does not have a feasible solution.')
        exit(0)

print('Solution:')
print('Objective value =', solver.Objective().Value())
offsets = dict()
minOffset = None
minRank = -1
maxOffset = None
maxRank = -1
fOffsets = open("_offsets", "w")
for rank in sorted(vars):
    if rank >= OFFSET_ABS_VAR:
        continue
    var = vars[rank]
    val = var.solution_value()
    if minOffset is None or minOffset > val:
        minOffset = val
        minRank = rank
    if maxOffset is None or maxOffset < val:
        maxOffset = val
        maxRank = rank
    fOffsets.write(str(rank) + " " + str(val) + "\n")
    offsets[rank] = val
fOffsets.close()
print("Min. offset:", minRank, "->", minOffset)
print("Max. offset:", maxRank, "->", maxOffset)

for arg in sys.argv[1:]:
    fOut = open(arg + ".harmonized", 'w')
    for line in open(arg, "r").readlines():
        line = line.rstrip()

        words = line.split(" ")
        if len(words) < 2:
            continue

        try:
            t = float(words[0])
            r = int(words[1])
        except ValueError:
            continue

        if r in offsets:
            words[0] = "%.4f" % (t + offsets[r])
            fOut.write(" ".join(words) + "\n")
    fOut.close()
    print("Wrote to", arg + ".harmonized")
