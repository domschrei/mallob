
import numpy as np
import random
import math
from array import array
from copy import deepcopy
import sys

num_runs = 100
steps_per_run = 300

cdfs = []
avg_cdf = [0 for x in range(steps_per_run)]

n = int(sys.argv[1])
r = int(sys.argv[2])
l = 0.95

# Modes: "full" "fixed_naive_u@r" "dynamic_u@r" "circular" "mallob"
mode = "mallob" 


prob_goalexists = 1 - l**n


for run in range(num_runs):

    p_visited = array('f')
    p_active = array('f')

    p_visited.append(1.0)
    p_active.append(1.0)
    for x in range(1, n):
        p_visited.append(0.0)
        p_active.append(0.0)

    if mode == "fixed_naive_u@r":
        out_neighbors = []
        for x in range(n):
            out_neighbors += [set()]
            while len(out_neighbors[x]) < r:
                randnode = random.choice(range(n))
                if randnode != x:
                    out_neighbors[x].add(randnode)
    if mode == "full":
        r = n-1
    if mode == "circular":
        out_neighbors = []
        for x in range(n):
            out_neighbors += [set()]
            y = int(x - r/2 + n) % n
            while len(out_neighbors[x]) < r:
                out_neighbors[x].add(y)
                y = (y+1) % n
                if y == x:
                    y = (y+1) % n
    
    if mode == "mallob":
        out_neighbors = []
        
        # 1st permutation: hamilton circle
        first_perm = [x for x in range(n)] 
        random.shuffle(first_perm)
        intrinsic_partners = []
        for x in range(n):
            idx = first_perm.index(x)
            partner = first_perm[(idx+1)%n]
            out_neighbors += [set()]
            out_neighbors[x].add(partner)
            intrinsic_partners += [partner]
        #print(intrinsic_partners)
        
        # following permutations
        permutations = []
        
        def is_valid(pos, val):
            if pos == val:
                return False
            if intrinsic_partners[pos] == val:
                return False
            for p in permutations:
                if p[pos] == val:
                    return False
            return True
        
        for i in range(1, r):
            p = [x for x in range(n)]
            random.shuffle(p)
            for x in range(n):
                if not is_valid(x, p[x]):
                    success = False
                    while not success:
                        x_swap = random.randrange(0, n-1)
                        if x_swap >= x:
                            x_swap += 1
                        
                        if is_valid(x, p[x_swap]) and is_valid(x_swap, p[x]):
                            success = True
                            p_swap = p[x_swap]
                    p[x_swap] = p[x]
                    p[x] = p_swap
            permutations += [p]
        
        for p in permutations:
            for x in range(n):
                if p[x] in out_neighbors[x]:
                    print("ERROR")
                    exit(1)
                out_neighbors[x].add(p[x])
    
        occ = dict()
        for i in range(len(out_neighbors)):
            if len(out_neighbors[i]) != r:
                print("ERROR")
                exit(1)
            string = "%i => " % i
            for nb in out_neighbors[i]:
                string += "%i " % nb
                if nb not in occ:
                    occ[nb] = []
                occ[nb] += [i]
            #print(string)
        for o in occ:
            if len(occ[o]) != r:
                print("ERROR")
                exit(1)
        #print(occ)
    #print(out_neighbors)

    """
    in_neighbors = dict()
    for x in range(n):
        in_neighbors[x] = set()

    for x in out_neighbors:
        for y in out_neighbors[x]:
            in_neighbors[y].add(x)
    """

    def compute():
        
        # unit vector
        I = np.ones((n))
        
        # transition matrix (activity transitions)
        A = np.zeros((n, n))
        for i in range(n):
            for j in range(n):
                if j in out_neighbors[i]:
                    A[i,j] = 1.0/r
        
        # initial distribution vector
        d = np.zeros((n))
        d[0] = 1.0
        
        # coveredness vector
        c = np.zeros((n))
        c[0] = 1.0
        
        # initial probability
        cdf = []
        
        for t in range(steps_per_run):
            cdf += [(1 - np.prod(I - (1-l)*c))] #/ prob_goalexists]
            #print(cdf[-1])
            d = A.dot(d)
            c = d + (I - d) * c
        
        return cdf

    def simulate():
        
        change = True
        it = 1
        sum_prob_hit = 0.0
        cdf = []
        while change and it <= steps_per_run:
            
            #print("\n*** Iteration",it,"***")
            
            prob_nonehit = 1
            for i in range(n):
                p_visited_and_goal = p_visited[i]*(1-l)
                prob_nonehit *= (1 - p_visited_and_goal)
            prob_hit = 1 - prob_nonehit
            # normalize by probability that any goal exists
            prob_hit = prob_hit / prob_goalexists
            
            cdf += [prob_hit]
            
            change = False
            prev_visited = deepcopy(p_visited)
            prev_active = deepcopy(p_active)
            p_visited = array('f')
            p_active = array('f')
            
            # Initialize
            for u in range(n):
                p_visited.append(prev_visited[u])
                p_active.append(0)
            
            # Do one jump from every node
            for u in range(n):
                u_active = prev_active[u]*(1.0/float(r))
                if u_active <= 0:
                    continue
                if mode == "full":
                    for v in range(n):
                        if v == u:
                            continue
                        # Not visited yet, a neighbor was active and it passed the ball
                        p_visited[v] += (1-prev_visited[v]) * u_active
                        # A neighbor was active and passed the ball
                        p_active[v] += u_active
                if mode == "fixed_naive_u@r" or mode == "circular" or mode == "mallob":
                    for v in out_neighbors[u]:
                        # Not visited yet, a neighbor was active and it passed the ball
                        p_visited[v] += (1-prev_visited[v]) * u_active
                        # A neighbor was active and passed the ball
                        p_active[v] += u_active
                if mode == "dynamic_u@r":
                    for c in range(r):
                        v = random.randrange(n)
                        while v == u:
                            v = random.randrange(n)
                        # Not visited yet, a neighbor was active and it passed the ball
                        p_visited[v] += (1-prev_visited[v]) * u_active
                        # A neighbor was active and passed the ball
                        p_active[v] += u_active
            
            change = True
            
            if sum(p_active) <= 0.99:
                print("ERROR: Global activity =",sum(p_active))
                exit(1)
            
            # Complete map
            #print(p_visited)
            #print(p_active)
            #print("sum over activity:",sum(p_active))
            #print("sum over coveredness:",sum(p_visited))
            
            #input()
            
            # Minimum, maximum
            #print(min(p_visited),max(p_visited))
            #print(len([x for x in p_visited if x <= 0]),"unvisited nodes")
            
            it += 1
            
        return cdf
    
    
    #cdf = simulate()
    cdf = compute()    
    cdfs += [cdf]


for i in range(steps_per_run):
    minimum = min([cdf[i] for cdf in cdfs])
    maximum = max([cdf[i] for cdf in cdfs])
    mean = sum([cdf[i] for cdf in cdfs])/len(cdfs)
    stddev = math.sqrt(1.0/(len(cdfs)-1) * sum([(cdf[i]-mean)**2 for cdf in cdfs]))
    print("%.8f %.8f %.8f %.8f" % (mean, minimum, maximum, stddev))
