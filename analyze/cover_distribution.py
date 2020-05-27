
import random
import math
from array import array
from copy import deepcopy

n = 511
r = 8
l = 0.95
goalsamples = 1000

p_visited = array('f')
p_active = array('f')

p_visited.append(1.0)
p_active.append(1.0)
for x in range(1, n):
    p_visited.append(0.0)
    p_active.append(0.0)

# Modes: "full" "fixed_naive_u@r" "dynamic_u@r" "circular" "mallob"
mode = "mallob" 

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
    for x in range(n):
        out_neighbors += [set()]
        out_neighbors[x].add(first_perm[x])
    
    def is_valid(pos, val):
        if pos == val:
            return False
        if first_perm[pos] == val:
            return False
        for p in permutations:
            if p[pos] == val:
                return False
        return True
    
    # following permutations
    permutations = []
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
            out_neighbors[x].add(p[x])

#print(out_neighbors)
    

"""
in_neighbors = dict()
for x in range(n):
    in_neighbors[x] = set()

for x in out_neighbors:
    for y in out_neighbors[x]:
        in_neighbors[y].add(x)
"""

change = True
it = 1
sum_prob_hit = 0.0
while change and it < 300:
    
    #print("\n*** Iteration",it,"***")
    
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
        u_active = prev_active[u]*(1/float(r))
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
    
    # Simulate hit probability
    probs = []
    for i in range(goalsamples):
        
        goalnodes = set()
        for j in range(round((1-l)*n)):
            randnode = random.randrange(n)
            while randnode in goalnodes:
                randnode = random.randrange(n)
            goalnodes.add(randnode)
        #print(goalnodes)
        
        # P(Not goal) = P(none of the goals was visited)
        prob_notgoal = 1
        for v in goalnodes:
            prob_notgoal *= (1-p_visited[v])
        # P(Goal) = 1-P(Not goal)
        prob_goal = 1 - prob_notgoal
        #print(goalnodes,"prob:",prob_goal)
        probs += [prob_goal]
    
    # Geometric mean
    prod = 1
    for p in probs:
        prod *= p
    prob_hit = prod**(1/len(probs))
    
    # Robust arithmetic mean (omitting 1st and last 10-quantile)
    probs.sort()
    prob_hit = 0
    for i in range(int(0.1*len(probs)), int(0.9*len(probs))+1):
        prob_hit += probs[i]
    prob_hit /= int(0.9*len(probs))+1-int(0.1*len(probs))
    
    #if prob_hit > 0.0001:
    change = True
    #sum_prob_hit += prob_hit
    
    # Hit probability
    #print("P( h =",it,") =",prob_hit,", accumulated",sum_prob_hit)
    print(prob_hit)
    
    # Complete map
    #print(p_visited)
    #print(p_active)
    #print("sum over activity:",sum(p_active))
        
    # Minimum, maximum
    #print(min(p_visited),max(p_visited))
    #print(len([x for x in p_visited if x <= 0]),"unvisited nodes")
    
    it += 1
