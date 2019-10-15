
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

# Increment mallob revision number
def update_revision(target, source, env):
    revision = "#define MALLOB_REVISION \"" + time.strftime("%Y-%m-%d_%H:%M:%S") + "\""
    with open("src/revision.c", "w") as f:
        f.write(str(revision))
    print revision

# Directories of hordesat
horde = "src/hordesat/incremental-hordesat/"
hordesolvers = "src/hordesat/"

# Source files
hordesat_sources = Glob(horde + "*.cpp") + Glob(horde + "utilities/*.cpp") + Glob(horde + "solvers/*.cpp") + Glob(horde + "sharing/*.cpp")
mallob_sources = Glob("src/*.cpp") + Glob("src/util/*.cpp") + Glob("src/data/*.cpp") + Glob("src/balancing/*.cpp")

# Build an object (.o) for each .cpp file of horde sources
hordeenv = get_default_env()
hordeenv.Append(CXXFLAGS = ["-fpermissive"])
hordesat_objects = []
for src in hordesat_sources:
    obj = hordeenv.Object("build/" + str(src).replace(".cpp",".o"), src,
            CPPPATH=[horde, hordesolvers + "minisat", hordesolvers + "lingeling"],
            LIBPATH=[hordesolvers + "/minisat/build/release/lib", hordesolvers + "/lingeling"],
            LIBS=["pthread", "minisat", "lgl", "z"])
    hordesat_objects += [obj]

# Build an object (.o) for each .cpp file of mallob sources
mallobenv = get_default_env()
mallob_objects = []
for src in mallob_sources:
    obj = mallobenv.Object("build/" + str(src).replace(".cpp",".o"), src,
            CPPPATH=['src', horde], 
            LIBPATH=[".", hordesolvers + "/minisat/build/release/lib", hordesolvers + "/lingeling"],
            LIBS=["horde", "pthread", "minisat", "lgl", "z"])
    mallob_objects += [obj]

# Build hordesat from objects
hordeenv = get_default_env()
hordeenv.Append(CXXFLAGS = ["-fpermissive"])
hordelib = hordeenv.Library("build/horde", 
        hordesat_objects,
        CPPPATH=[horde, hordesolvers + "minisat", hordesolvers + "lingeling"],
        LIBPATH=[hordesolvers + "/minisat/build/release/lib", hordesolvers + "/lingeling"],
        LIBS=["pthread", "minisat", "lgl", "z"])

# Build mallob from objects
mallob = mallobenv.Program('build/mallob', 
        mallob_objects,
        CPPPATH=['src', horde], 
        LIBPATH=["build/", hordesolvers + "/minisat/build/release/lib", hordesolvers + "/lingeling"],
        LIBS=["horde", "pthread", "minisat", "lgl", "z"])
Depends(mallob, hordelib)

version = Command("version", [], update_revision)
for src in mallob_sources:
    Depends(src, version)

Default(mallob)
