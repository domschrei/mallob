
# Mallob SAT Engine

This directory contains the sources of the Mallob SAT Engine.

Note that some of the sources have originated from MIT-licensed HordeSat, a massively parallel portfolio satisfiability solver, and have since then been heavily modified, extended, and/or rewritten.
For more information on original HordeSat please visit https://baldur.iti.kit.edu/hordesat/.

## Overview

The MSE can be launched in two different modes of operation: Threaded and forked mode.
In threaded mode, the MSE is run as a part of the calling process and spawns separate threads for the core solvers. The entry point for this mode of operation is `execution/engine.cpp` (called by Mallob from `job/threaded_sat_job.cpp`).
In forked mode, the MSE is executed as a separate process. In this case, the entry point is `main.cpp` (in this directory), executed as a separate process from `job/sat_process_adapter.cpp` which, in turn, is called by `job/forked_sat_job.cpp`.

The principal interface of the MSE is `execution/engine.cpp`. Note that this interface does not feature any kind of MPI communciation, nor are there any MPI calls done from this entry point. Instead, communication is done separately by Mallob's main thread within the virtual methods `Job::communicate(*)` overridden by ThreadedSatJob and ForkedSatJob. In particular, the clause sharing logic is implemented in `job/anytime_sat_clause_communicator.cpp`.

## Core Solvers

The interfaces to core solvers can be found in `solvers/`. The common interface `solvers/portfolio_solver_interface.hpp` is overridden by the different solvers.
Create new solver interfaces by using existing interfaces as an analogy.

## Clause Sharing

While communication itself is done in `job/`, the internal solver-side data structures for clause buffering and filtering reside in `sharing/`.
The entry point is `sharing/sharing_manager.cpp`. Each time a solver exports a clause, `onProduceClause` is executed, and each time clauses are to be imported to solvers, a `digest*` method is executed.  
