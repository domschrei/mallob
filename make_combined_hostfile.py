#!/usr/bin/env python3
from os import listdir
from os.path import isfile, join
import re

HOST_FILE_PATH="/tmp/"
external_ips = []
onlyfiles = [f for f in listdir(HOST_FILE_PATH) if isfile(join(HOST_FILE_PATH, f))]
combined_text = ""

slots_per_ip = dict()

for filename in onlyfiles:
    if "hostfile" not in filename:
        continue
    for line in open(join(HOST_FILE_PATH, filename)).readlines():
        match = re.match(r'([0-9\.]+) slots=([0-9]+)', line.rstrip())
        if match:
            print(match.group(0))
            ip = match.group(1)
            nslots = int(match.group(2))
            if ip not in slots_per_ip:
                slots_per_ip[ip] = 0
            slots_per_ip[ip] += nslots

with open("combined_hostfile", "w") as f:
    for ip in slots_per_ip:
        f.write(ip + " slots=" + str(slots_per_ip[ip]) + "\n")

"""
for filename in onlyfiles:
    if "hostfile" in filename:
        with open(join(HOST_FILE_PATH, filename)) as f:
            combined_text+=f.read()
with open("combined_hostfile", "w") as f:
    f.write(combined_text)
"""
