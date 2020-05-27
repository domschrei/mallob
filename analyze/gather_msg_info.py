
import sys
from os import listdir
from os.path import isfile, join
import matplotlib.pyplot as plt
import random
import re

class Message:
    
    def __init__(self, id, src, dest, tag, starttime):
        self.id = id
        self.src = src
        self.dest = dest
        self.tag = tag
        self.starttime = starttime
        self.endtime = None
    
    def add_sent_info(self, endtime):
        self.endtime = endtime
        
    def get_duration(self):
        if self.endtime:
            return self.endtime - self.starttime
        else:
            return float('nan')


# For each rank, read log and extract all sent messages into dict
logdir = sys.argv[1]
files = [f for f in listdir(logdir) if isfile(join(logdir, f))]
messages = dict()
for f in files:
    match = re.search(r'log_[0-9]+.[0-9]+', f)
    if not match:
        continue
    
    
    for line in open(logdir + "/" + f, "r").readlines():
        line = line.replace("\n", "")
        
        match = re.search(r'^\[([0-9]+\.[0-9]+) / ([0-9]+\.[0-9]+)\] \[([0-9]+)\] Msg ID=([0-9]+) dest=([0-9]+) tag=([0-9]+) starttime=([0-9]+\.[0-9]+) (.*)', line)
        if match:
            # Message start
            time = match.group(2)
            rank = match.group(3)
            msgid = int(match.group(4))
            dest = match.group(5)
            tag = match.group(6)
            starttime = match.group(7)
            msg = Message(int(msgid), int(rank), int(dest), int(tag), float(starttime))
            messages[msgid] = msg
            print(msgid)
        else:
            match = re.search(r'^\[([0-9]+\.[0-9]+) / ([0-9]+\.[0-9]+)\] \[([0-9]+)\] Message ID=([0-9]+) isent', line)
            if match:
                # Message sent
                time = match.group(2)
                rank = match.group(3)
                msgid = int(match.group(4))
                messages[msgid].add_sent_info(float(time))

# Flatten from dict to lists
message_list = []
durations = []
sendranks = []
recvranks = []
tags = []
starttimes = []
for id in messages:
    msg = messages[id]
    message_list += [msg]
    durations += [msg.get_duration()]
    sendranks += [msg.src]
    recvranks += [msg.dest]
    tags += [msg.tag]
    starttimes += [msg.starttime]
norm_durations = [1-(d-min(durations))*(max(durations)-min(durations)) for d in durations]
print(norm_durations)

fig, axes = plt.subplots(2, 2)
plt.gray()
axes[0,0].scatter(tags, durations)
axes[0,0].set_title("Tags - durations")
axes[0,1].scatter([r-0.15+0.3*random.random() for r in sendranks], [r-0.15+0.3*random.random() for r in recvranks], c=norm_durations, marker="+", alpha=0.5)
axes[0,1].set_title("Sendrank - Recvrank (color: duration)")
axes[1,0].scatter(starttimes, durations)
axes[1,0].set_title("Starttime - duration")
axes[1,1].scatter([r-0.15+0.3*random.random() for r in sendranks], durations)
axes[1,1].set_title("Sendrank - duration")

plt.show()

