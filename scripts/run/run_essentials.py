
import os
import sys
import random
import json
import time

def wait_for_json(file):
    while True:
        if os.path.isfile(file):
            try:
                j = json.load(open(file, 'r'))
                os.remove(file)
                break
            except:
                pass
        time.sleep(0.1)
    return j

d = ".api/jobs.0/"
hardness_factor = 1000

j = {
    "user": "admin", 
    "name": "job-1",
    "files": ["instances/r3sat_200.cnf"], 
    #"file": "instances/incremental_sat_01.cnf", 
    "priority": 1.000, 
    "wallclock-limit": "0", 
    "cpu-limit": "0", 
    "incremental": True 
}

# Write initial job JSON
with open(d + "new/admin.job-1.json", 'w') as outfile:
    json.dump(j, outfile)

# Main loop
models = []
r = 1
while True:
    filename = "admin.job-" + str(r) + ".json"

    # Wait for result
    j = wait_for_json(d + "done/" + filename)
    
    # Result arrived in j: extract model
    if j['result']['resultstring'] == 'SAT':
        model = j['result']['solution']
        models += [model]
        print(model)
        
    elif j['result']['resultstring'] == 'UNSAT':
        print("Found all " + str(len(models)) + " models")
        break # all models found
    
    # Write next iCNF
    with open("instances/incremental.cnf", "w") as f:
        f.write("p cnf " + str(len(models[-1])) + " " + str(hardness_factor) + "\n")
        for x in range(hardness_factor):
            for i in range(1, len(models[-1])):
                f.write(str(-models[-1][i]) + " ")
            f.write(" 0\n")
    
    # Write next job JSON file
    j["file"] = "instances/incremental.cnf"
    j["name"] = "job-" + str(r+1)
    j["precursor"] = "admin.job-" + str(r)
    with open(d + "new/admin.job-" + str(r+1) + ".json", 'w') as outfile:
        json.dump(j, outfile)
    
    r += 1


# Notify end of job
j["name"] = "job-" + str(r+1)
j["precursor"] = "admin.job-" + str(r)
j["done"] = True
with open(d + "new/admin.job-" + str(r+1) + ".json", 'w') as outfile:
    json.dump(j, outfile)


print(models)

#j = json.load(open(d + "new/", 'r'))
#print(j)
