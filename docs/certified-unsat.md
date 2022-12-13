
## Distributed UNSAT Proof Production

This version of Mallob is written to produce proofs of unsatisfiability for distributed SAT solvers using a modified version of [CaDiCaL](https://github.com/RandomActsOfGrammar/cadical).
Each CaDiCaL instance produces a log of the clauses it learned in the LRAT format.
These are then combined into a single LRAT by our proof composers (sequentially in the `tools` directory, or fully distributed within Mallob itself) to form a single, checkable LRAT proof.

Our proof combination relies on a particular formula being used to generate clause identifiers in each instance, described [here](https://github.com/RandomActsOfGrammar/cadical#distributed-solver-implementation-notes).
We use this to ensure each clause identifier is unique across all solvers.
Currently, CaDiCaL is the only solver producing LRAT proofs with clause identifiers produced according to this formula, and thus is the only solver that can be used for distributed proof production.
Other solvers could be modified to write LRAT proofs with clause identifiers produced according to this format, in which case they could be used to add more diversity to the solver portfolio while still producing proofs.
