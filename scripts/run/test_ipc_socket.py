
from multiprocessing.connection import Client
import glob
import sys

from time import sleep
from os import mkfifo, remove

# ASCII character to send in order to close an active connection.
ASCII_CHAR_CANCEL = 24;

# Takes a usual Python3 string and sends it over the given connection.
def send_string(conn, string):
    ascii = string.encode('ascii')
    conn.send_bytes(ascii)

# Returns a string of (ASCII) characters received over the given connection.
def recv(conn):
    return conn.recv_bytes().decode('ascii')

def write_raw_integers(f, ints):
    for x in ints:
        f.write((x).to_bytes(4, byteorder=sys.byteorder, signed=True))

# Returns a string specifying a path to a Mallob IPC socket, or None.
def find_mallob_socket():
    files = glob.glob('/tmp/mallob_*.0.sk')
    if files == []:
        return None
    return files[-1]


formula_path = "instances/g2-T83.2.1.cnf"
named_pipe_path = "/tmp/myapp.pipe"

json_introduce = '{"user": "admin", "name": "mono-job", "file": "' + formula_path + '", "priority": 1.0, "application": "SAT"}'
#json_introduce_namedpipe = '{"user": "admin", "name": "mono-job", "file": "' + named_pipe_path + '", "priority": 1.0, "application": "SAT"}'
json_introduce_namedpipe = '{"user": "admin", "name": "mono-job", "file": "' + named_pipe_path + '", "configuration": {"content-mode": "raw"}, "priority": 1.0, "application": "SAT"}'
json_interrupt = '{"interrupt": true, "user": "admin", "name": "mono-job", "application": "SAT"}'

#formula = open(formula_path, "r").read()
mkfifo(named_pipe_path)
formula_raw = [9, -1, 2, 0, -2, 3, 0, -3, 1, 0]



# Open connection
conn = Client(find_mallob_socket())

# Submit a job over the active connection
send_string(conn, json_introduce) #json_introduce_namedpipe)

# Pipe formula into named pipe
named_pipe = open(named_pipe_path, 'wb')
write_raw_integers(named_pipe, formula_raw)
named_pipe.close()

# Wait a second
sleep(1)

# Interrupt the current job
send_string(conn, json_interrupt)

# Wait for a response
response = recv(conn)
print("Received:", response)

# Send notification which lets Mallob close the connection (important!)
send_string(conn, chr(ASCII_CHAR_CANCEL))

# Close connection yourself
conn.close()

# Clean up named pipe
remove(named_pipe_path)
