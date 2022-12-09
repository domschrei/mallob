#!/bin/bash

# Runs the command with valgrind with the massif tool, profiling heap usage.
# The resulting files can be visualized with the Massif Visualizer.

PATH=build:.:$PATH RDMAV_FORK_SAFE=1 exec -a=/usr/bin/valgrind /usr/bin/valgrind --tool=massif $@
