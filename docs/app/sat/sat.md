
# SAT Application Engine (MallobSat)

This page describes general usage of MallobSat, Mallob's integrated distributed SAT solving engine. It is commonly enabled via `-mono-app=SAT` and is also used as a backend in most other application engines of Mallob.

For quickstarting, there are several MallobSat presets available in `config/presets/`, such as `satcomp26-safe` (with real-time proof checking), `sat-incremental` (for incremental SAT solving), and `palrup-*` (for scalable production and checking of parallel UNSAT proofs).

## Input Formats

The input can be provided (a) as a plain DIMACS CNF text file, (b) as a compressed (.lzma / .xz) DIMACS CNF file, or (c) as a compact binary file.
CNF files may contain lines of the form `a <lit1> <lit2> ... 0` to indicate an **incremental SAT call**, where `<lit1>`, `<lit2>` etc. are assumption literals for incremental solving.
For binary files, Mallob reads clauses as integer sequences with separation zeroes in between; the special integer INT32_MAX (2147483647) separates the clause literals from the sequence of assumption integers, and then another zero signals that the description is complete. This integer sequence is also how the field "literals" in manual job submission JSONs should be used (see below at [Introducing a job](#introducing-a-job)).

## Producing Monolithic Proofs of Unsatisfiability

To enable proof production, just set the option `-proof=path/to/final/compressed/prooffile.lrat` together with `-mono=path/to/input.cnf`. You also need to set a log directory with `-proof-dir=path/to/dir` where intermediate files will be written to on each machine. The final proof is output in compressed LRAT format.

CaDiCaL is currently the only supported solver backend for parallel/distributed proof production. However, it is possible to employ other solvers as long as their only purpose is to find a satisfying assignment (i.e., exported clauses and unsatisfiability results from these solvers are discarded). See *Portfolio Tweaking* below.

You can check the output proof with the standalone LRAT checker that comes with Mallob if you set `-DMALLOB_BUILD_LRAT_MODULES=1` at build time:
```bash
build/standalone_lrat_checker path/to/input.cnf path/to/final/compressed/prooffile.lrat
```
Further synergies are possible; you can set `-uninvert=0` for Mallob and `--reversed` for the checker to avoid one entire I/O pass over the proof that "uninverts" its lines.
You can use the [`drat-trim`](https://github.com/marijnheule/drat-trim) tool suite to decompress a proof; note that you need to `#define MODE 2` (1=DRAT, 2=LRAT) in `decompress.c` before building.

## Real-time Proof Checking

Proof production can be costly and bottlenecked by the I/O bandwidth of the single process which needs to write the entire proof. A more scalable approach is to check all proof information on-the-fly, without writing it to disk, and to transfer clause soundness guarantees across machines via hash-based fingerprints. This is explained in detail in our [2024 SAT publication](https://dominikschreiber.de/papers/2024-sat-trusted-pre.pdf) and our [2026 TACAS publication](https://satres.kikit.kit.edu/papers/2026-tacas-distrincproof.pdf), where we extend the framework to incremental SAT solving.

To enable the setup involving the latest, incremental ImpCheck (which is also more efficient than original ImpCheck), execute the command chain fetching and building ImpCheck in [`scripts/setup/sat-setup.sh`](../scripts/setup/sat-setup.sh). Then use Mallob's option `-otfc=1` (without any `-proof*` options) to enable on-the-fly checking and `-otfci=1` to ensure that Mallob expects the _incremental_ ImpCheck programs. Again, only CaDiCaL is supported for UNSAT whereas any solver can be employed for boosting satisfying assignments. By default, found satisfying assignments are also validated, which can be disabled via `-otfcm=0`.

Let's assume you get the following output from a proof checking process:
```
c 0.059 0 <Reader> [T] Initialized job #230 {instances/r3unsat_300.cnf} in 0.004s: size 5127
...
c 6.164 1 <#230> S6.0 IMPCHK_CONFIRM 1280 20 8791e38feb111d9094da61be8651478a
c 6.164 1 <#230> S6.0 Use iimpcheck_confirm -key-seed=13805254743912277295 to confirm fingerprint
```
The `IMPCHK_CONFIRM` line indicates that ImpCheck validated a result of code 20 (UNSAT, 10=SAT) on a formula with 1280 clauses; the appended fingerprint serves as a witness for this result.
To be extra safe (e.g., if you are suspecting garbled or tampered-with logging output), accordingly execute a command like this to validate the output fingerprint:
```bash
build/iimpcheck_confirm -key-seed=13805254743912277295 -formula=instances/r3unsat_300.cnf -result=20 -sig=8791e38feb111d9094da61be8651478a
```
For incremental problems, Mallob will write a file `witness.#<jobid>` to the directory specified via the `-log` option (if existent) and a subsequent check of all results works as follows:
```bash
build/iimpcheck_confirm -key-seed=13805254743912277295 -formula=instances/r3unsat_300.cnf -witness=path/to/witness.#*
```

**Note:** On-the-fly checking can also be used in Mallob's scheduled mode of operation. Globally unique clause IDs are ensured by adding a large offset times $x$ to a new solver thread's clause ID counter if the job has already experienced $x$ _balancing epochs_, i.e., received $x$ volume updates, since its initialization. The offset is chosen in such a way that 10,000 solvers each producing 10,000 clauses per second can run for 10,000 seconds before they may begin overlapping with clause IDs from the next balancing epoch. `ImpCheck` notices and reports any errors that would result from such a corner case.

## Producing Distributed Proofs of Unsatisfiability

To enable proof production in the PalRUP framework run Mallob with options `-palrup=1 -proof-dir=path/to/palrup/proof`. The given path can be on local disks, if Mallob is run on a distributed system. To output the proof in a readable text format (instead of variable byte length coded binary format) set the option `-palrup-binary=0`.

Mallob supports checking a produced PalRUP proof immediatly after solving utilizing [PalRUP-Check](https://github.com/rubenGoetz/PalRUP-Check). To enable proof checking, build Mallob with the `-DMALLOB_APP_PALRUPCHECK=1` option. To check a PalRUP proof, run Mallob with the options `-palrup-check=1 -palrup-check-dir=path/to/communication/`. The given path must be accessable by all processes, i.e. located in a parallel file system if run on distributed systems. After a proof was validated successfully (unsuccessfully), a `success.palrup` (`failure.palrup`) file will be created in Mallob's log directory.

As described above, a PalRUP proof can be stored on local disks when it was created on a distributed system. If this is the case, make sure to run Mallob with the `-palrup-use-local-disks=1` option.

Non-default options can be passed to the PalRUP-Checker by setting the corresponding Mallob options. See [options.hpp](../src/app/palrupcheck/options.hpp) for details.

## Portfolio Tweaking

Mallob allows to customize the employed SAT solver backends and some of their flavors. This is done with the `-satsolver` option, which expects a string representing the solver backends to cycle over. The option also allows to specify a "lasso word", i.e., a regex-like expression that consists of a finite prefix followed by an infinitely looping sequence. Here are some examples:
```bash
... -satsolver='c' # CaDiCaL only.
... -satsolver='kcl' # Kissat, CaDiCaL, Lingeling, Kissat, CaDiCaL, Lingeling, Kissat, ...
... -satsolver='k(c_)*' # One Kissat, then only plain (_) CaDiCaL. Always put brackets around the argument of '*'!
... -satsolver='kCLCLcl' # Capital letters indicate using truly incremental SAT solving for incremental jobs
... -satsolver='l+(c!){37}' # One Lingeling configured for satisfiable instances (+), then 37 LRAT-producing (!) CaDiCaLs, repeat
... -satsolver='(c!){37}k+((c!){37}l+)*' # As above, but replacing the 1st Lingeling with Kissat
... -satsolver='(c!){37}k+[[c!]{37}l+]w' # Alternative notation with squared brackets and automaton-style "omega" (w) to avoid issues with bash
```

## Advanced Tweaking

Coming soon: Documentation on the JSON-based SAT solver configuration rules in `config/sat/`, activated via `-sat-config-dirs` and/or `-sat-config-files`.
