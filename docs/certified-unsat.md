
# Distributed UNSAT Proof Production

This version of Mallob is written to produce proofs of unsatisfiability for distributed SAT solvers using a modified version of [CaDiCaL](https://github.com/RandomActsOfGrammar/cadical).
Each CaDiCaL instance produces a log of the clauses it learned in the LRAT format.
These are then combined into a single LRAT by our proof composers (sequentially in the `tools` directory, or fully distributed within Mallob itself) to form a single, checkable LRAT proof.

Our proof combination relies on a particular formula being used to generate clause identifiers in each instance, described [here](https://github.com/RandomActsOfGrammar/cadical#distributed-solver-implementation-notes).
We use this to ensure each clause identifier is unique across all solvers.
Currently, CaDiCaL is the only solver producing LRAT proofs with clause identifiers produced according to this formula, and thus is the only solver that can be used for distributed proof production.
Other solvers could be modified to write LRAT proofs with clause identifiers produced according to this format, in which case they could be used to add more diversity to the solver portfolio while still producing proofs.

## Usage

### Building

To build Mallob with support for certified UNSAT, you need to build an LRAT-producing modification of CaDiCaL. Execute the script `fetch_and_build_sat_solvers.sh` in the `lib/` directory with the letter "p" (e.g., `bash fetch_and_build_sat_solvers.sh kply`) to fetch and build this CaDiCaL.

Mallob needs to be built with `-DMALLOB_CERTIFIED_UNSAT=1` set. This links the Mallob sources with the CaDiCaL library in `lib/lrat-cadical` instead of `lib/cadical`.

### Execution

As of now, the described build only supports the `-mono` mode of operation (i.e., solving a single instance only).

The following options are relevant for certified UNSAT:

* `-certified-unsat=1`: This option must be enabled to produce proofs.
* `-distributed-proof-assembly=1`: Turn on distributed proof assembly. If turned off, the system will terminate after each solver has written its individual proof file.
* `-log=<logdir>`: Important option since it also sets the base location for the proof files directory on each process.
* `-mempanic=0`: Turn off memory panic. Essential for correct functionality of proof logging.
* `-extmem-disk-dir=<disk-dir>`: Set the directory where virtual disk files should be placed. The disk where the specified directory lies is also the one which will be filled with the content of the external priority queues. Set this to a directory on the best performing disk which is available and still offers enough space.
* `-proof-output-file=<proof-file>`: Specify the path and name of the final LRAT output file. This file is only output at rank zero.
* `-sat-preprocessor=<executable>`: Specify a path to an executable which is used to preprocess the input. For example, you can use `scripts/run/preprocess_cnf.sh`. **If this option is not enabled, the input must be an accordingly preprocessed file.**
* `-interleave-proof-merging=1`: Interleave pruning and merging of proofs. This skips writing all the intermediate pruned proofs on disk and reading them after pruning again.
