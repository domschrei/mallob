
import sys
import random

num_vars = int(sys.argv[1])
num_cls = int(4.27 * num_vars) # phase transition of 3-SAT according to Zhao03

if len(sys.argv) > 2:
    random.seed(sys.argv[2])

out = "p cnf " + str(num_vars) + " " + str(num_cls) + "\n"

for c in range(num_cls):
    for i in range(3):
        lit = 0
        while lit == 0:
            lit = random.randrange(-num_vars, num_vars+1)
        out += str(lit) + " "
    out += "0\n"

print(out)
