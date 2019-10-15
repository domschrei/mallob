
import os
import subprocess
import hashlib
import re
import os.path
import fileinput
import time

# Compile flags
flags = "-g -O3 -Wall -fmessage-length=0"

def get_default_env():
    env = Environment()
    env['ENV']['TERM'] = os.environ['TERM'] # colored gcc output
    env.Replace(CXX = "mpic++") # compile with mpic++
    env.Append(CXXFLAGS = Split(flags)) # compile flags
    return env

# Directories of hordesat
horde = "src/hordesat/incremental-hordesat/"
hordesolvers = "src/hordesat/"

# Source files
hordesat_sources = Glob(horde + "*.cpp") + Glob(horde + "utilities/*.cpp") + Glob(horde + "solvers/*.cpp") + Glob(horde + "sharing/*.cpp")
mallob_sources = Glob("src/*.cpp") + Glob("src/util/*.cpp") + Glob("src/data/*.cpp") + Glob("src/balancing/*.cpp")

# Build hordesat
hordeenv = get_default_env()
hordeenv.Append(CXXFLAGS = ["-fpermissive"])
hordelib = hordeenv.Library("horde", 
        hordesat_sources,
        CPPPATH=[horde, hordesolvers + "minisat", hordesolvers + "lingeling"],
        LIBPATH=[hordesolvers + "/minisat/build/release/lib", hordesolvers + "/lingeling"],
        LIBS=["pthread", "minisat", "lgl", "z"])

# Increment mallob revision number
def update_revision(target, source, env):
    revision = "#define MALLOB_REVISION \"" + time.strftime("%Y-%m-%d_%H:%M:%S") + "\""
    with open("src/revision.c", "w") as f:
        f.write(str(revision))
    print revision

# Build mallob
mallobenv = get_default_env()
mallob = mallobenv.Program('mallob', 
        mallob_sources,
        CPPPATH=['src', horde], 
        LIBPATH=[".", hordesolvers + "/minisat/build/release/lib", hordesolvers + "/lingeling"],
        LIBS=["horde", "pthread", "minisat", "lgl", "z"])

version = Command("version", [], update_revision)
for src in mallob_sources:
    Depends(src, version)

Default(mallob)
