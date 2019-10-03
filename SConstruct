
import os
import subprocess

flags = "-g -O3 -Wall -fmessage-length=0"

def get_default_env():
    env = Environment()
    env['ENV']['TERM'] = os.environ['TERM']
    env.Replace(CXX = "mpic++")
    env.Append(CXXFLAGS = Split(flags))
    return env

horde = "src/hordesat/incremental-hordesat/"
hordesolvers = "src/hordesat/"

hordesat_sources = Glob(horde + "*.cpp") + Glob(horde + "utilities/*.cpp") + Glob(horde + "solvers/*.cpp") + Glob(horde + "sharing/*.cpp")
mallob_sources = Glob("src/*.cpp") + Glob("src/util/*.cpp") + Glob("src/data/*.cpp")

hordeenv = get_default_env()
hordeenv.Append(CXXFLAGS = ["-fpermissive"])
hordelib = hordeenv.Library("horde", 
        hordesat_sources,
        CPPPATH=[horde, hordesolvers + "minisat", hordesolvers + "lingeling"],
        LIBPATH=[hordesolvers + "/minisat/build/release/lib", hordesolvers + "/lingeling"],
        LIBS=["pthread", "minisat", "lgl", "z"])

mallobenv = get_default_env()
mallob = mallobenv.Program('mallob', 
        mallob_sources,
        CPPPATH=['src', horde], 
        LIBPATH=[".", hordesolvers + "/minisat/build/release/lib", hordesolvers + "/lingeling"],
        LIBS=["horde", "pthread", "minisat", "lgl", "z"])
