
**Note D. Schreiber 2022-12-13**: This directory contains a project developed by Dawn Michaelson @ Amazon. It is MIT-licensed (see file `LICENSE` in this directory) and features tools for the production of proofs with Mallob.
The original description of the repository follows.


<hr/>


# Composition of Distributed UNSAT Proofs
When a distributed SAT solver produces a result, can we trust it?
If the result is `SAT`, we can check the assignment produced to see if it is valid.
However, if the result is `UNSAT`, the solver gives us no more information to assure us of its correctness.

We propose to build a proof of unsatisfiability for distributed, clause-sharing SAT solvers by combining proofs produced by the individual SAT solvers.
Each individual SAT solver produces an [LRAT](https://www.cs.utexas.edu/users/marijn/publications/lrat.pdf) (or [FRAT](https://lmcs.episciences.org/9357/pdf) with all proof hints for added clauses) proof.
We then combine these proofs to produce a single, sequential LRAT proof to show the initial problem was unsatisfiable.

Versions of [Mallob](https://github.com/RandomActsOfGrammar/mallob) and [CaDiCaL](https://github.com/RandomActsOfGrammar/cadical) have been modified to produce instance proofs that we can combine.


## This Repository
This repository builds three programs:
* `compose-proofs`:
  Combines the instance proofs produced by each SAT solver into a single, sequential proof.
  This also prunes the joined file so the final result only includes clauses that will ever be needed, and tells the proof checker to delete clauses immediately after their last use.
  We require the clause identifiers in the files being combined to conform to the clause identifier formula described below.
* `renumber-proofs`:
  Renumbers the clauses in a proof to be sequential.
  Our proof composer does not, in general, result in the clause identifiers being in order.
  The `lrat-check` LRAT proof checker (part of the [drat-trim](https://github.com/marijnheule/drat-trim) repository) assumes the clause identifers are in order numerically, so we run this on the result of composing the instance proofs.
* `dratify`:
  Turn an LRAT proof into a DRAT proof.
  This is only for testing purposes, to compare the speeds of checking the LRAT and DRAT versions of the proof.


## Clause ID Formula
Because we have several SAT solvers sharing clauses, we need the clause identifiers produced by each solver to be unique from those produced by any other solver.
We number the original clauses sequentially starting with `1` as is standard.
Each learned clause is then numbered according to the formula `o + i + t * k`:
* `o`:  Number of original clauses
* `t`:  Total solver instances
* `i`:  Current instance number (`1`, `2`, ... `t`)
* `k`:  Number of clauses generated in the current instance (`0`, `1`, `2`, ...)

Our proof composer assumes the instance proofs given to it use this formula for the clause identifiers.
This is necessary for the way it checks for clause dependencies being satisfied in the proof already in joining the files.
If this formula is not used, it will throw an error for clauses being found in the wrong files.
