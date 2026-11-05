
# SAT Application Engine (MallobSat)

This page describes general usage of MallobSat, Mallob's integrated distributed SAT solving engine. It is commonly enabled via `-mono-app=SAT` and is also used as a backend in most other application engines of Mallob.

For quickstarting, there are several MallobSat presets available in `config/presets/`, such as `satcomp26-safe` (with real-time proof checking), `sat-incremental` (for incremental SAT solving), and `palrup-*` (for scalable production and checking of parallel UNSAT proofs).

See also the [SATWITHPRE engine](../satwithpre/README.md), which combines the MallobSat engine with a number of preprocessing actors and, as such, hosts some of the currently best performing SAT solving methods in Mallob(Sat).


## Input Formats

The input can be provided (a) as a plain DIMACS CNF text file, (b) as a compressed (.lzma / .xz) DIMACS CNF file, or (c) as a compact binary file.
CNF files may contain lines of the form `a <lit1> <lit2> ... 0` to indicate an **incremental SAT call**, where `<lit1>`, `<lit2>` etc. are assumption literals for incremental solving.
For binary files, Mallob reads clauses as integer sequences with separation zeroes in between; the special integer INT32_MAX (2147483647) separates the clause literals from the sequence of assumption integers, and then another zero signals that the description is complete. This integer sequence is also how the field "literals" in manual job submission JSONs should be used (see [execute.md: Introducing a job](../../execute.md#introducing-a-job)).


## Proof Technology

MallobSat offers a range of options for logging and/or checking proofs of unsatisfiability for highest confidence in obtained results and the option to analyze UNSAT results.

### Monolithic Proofs of Unsatisfiability

> [!TIP]
> **Preset:** `scripts/run/mallob_local.sh $(config/presets/monolithic-proof) -mono=path/to/f.cnf`  
> ~ writes inverted LRUP proof to `proof.lrat`  
> ~ check with (e.g.): `build/standalone_lrat_checker path/to/f.cnf proof.lrat`  
> ~ performance tweak: add Mallob option `-uninvert-proof=0` and checker option `--reversed`

We call a proof _monolithic_ if it mimics the proof emitted by a sequential SAT solver, i.e., it is comprised of one single, linear file that combines the reasoning of all involved solver threads (as far as needed).

To enable monolithic proof production, just set the option `-proof=path/to/final/compressed/prooffile.lrat` together with `-mono=path/to/input.cnf`. You also need to set a log directory with `-proof-dir=path/to/dir` where intermediate files will be written to on each machine. The final proof is output in compressed LRAT format.

CaDiCaL is currently the only supported solver backend for parallel/distributed proof production. However, it is possible to employ other solvers as long as their only purpose is to find a satisfying assignment (i.e., exported clauses and unsatisfiability results from these solvers are discarded). See [Configuring the Portfolio](#configuring-the-portfolio) below.

You can check the output proof with the standalone LRAT checker that comes with Mallob if you set `-DMALLOB_BUILD_LRAT_MODULES=1` at build time:
```bash
build/standalone_lrat_checker path/to/input.cnf path/to/final/compressed/prooffile.lrat
```
Further synergies are possible; you can set `-uninvert=0` for Mallob and `--reversed` for the checker to avoid one entire I/O pass over the proof that "uninverts" its lines.
You can use the [`drat-trim`](https://github.com/marijnheule/drat-trim) tool suite to decompress a proof; note that you need to `#define MODE 2` (1=DRAT, 2=LRAT) in `decompress.c` before building.

### Real-time Proof Checking

> [!TIP]
> **Preset:** `scripts/run/mallob_local.sh $(config/presets/satcomp26-safe) -mono=path/to/f.cnf`

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

### PalRUP: Distributed Proofs of Unsatisfiability

> [!TIP]
> **Presets:**  
> ~ `scripts/run/mallob_local.sh $(config/presets/palrup-solve) -mono=path/to/f.cnf` - solve, write proof to `./proofs/`  
> ~ `scripts/run/mallob_local.sh $(config/presets/palrup-check) -mono=path/to/f.cnf -proof-dir=path/to/proof/` - check produced proof  
> ~ `scripts/run/mallob_local.sh $(config/presets/palrup-solve-check) -mono=path/to/f.cnf` - solve, produce & immediately check proof

To enable _natively parallel_ proof production in the PalRUP framework, run Mallob with options `-palrup=1 -proof-dir=path/to/palrup/proof`. The given path can be on local disks, if Mallob is run on a distributed system. To output the proof in a readable text format (instead of variable byte length coded binary format) set the option `-palrup-binary=0`.

Mallob supports checking a produced PalRUP proof immediatly after solving utilizing [PalRUP-Check](https://github.com/rubenGoetz/PalRUP-Check). To enable proof checking, build Mallob with the `-DMALLOB_APP_PALRUPCHECK=1` option. To check a PalRUP proof, run Mallob with the options `-palrup-check=1 -palrup-check-dir=path/to/communication/`. The given path must be accessable by all processes, i.e. located in a parallel file system if run on distributed systems. After a proof was validated successfully (unsuccessfully), a `success.palrup` (`failure.palrup`) file will be created in Mallob's log directory.

As described above, a PalRUP proof can be stored on local disks when it was created on a distributed system. If this is the case, make sure to run Mallob with the `-palrup-use-local-disks=1` option.

Non-default options can be passed to the PalRUP-Checker by setting the corresponding Mallob options. See [options.hpp](../src/app/palrupcheck/options.hpp) for details.


## Solver Configuration

For advanced usage and configuration of MallobSat's solver threads, we offer a rich, two-stage interface for SAT solver configuration.

### Configuring the Portfolio

The first stage of configuration is that Mallob allows to customize the sequence of employed SAT solver _backends_ and some of their _flavors_. This is done with the `-satsolver` option, which expects a string representing the solver backends to cycle over. The option also allows to specify a "lasso word", i.e., a regex-like expression that consists of a finite prefix followed by an infinitely looping sequence. Here are some examples:
```bash
... -satsolver='c' # CaDiCaL only.
... -satsolver='kcl' # Kissat, CaDiCaL, Lingeling, Kissat, CaDiCaL, Lingeling, Kissat, ...
... -satsolver='k_' # Search-only (_) Kissat, i.e., no pre-/inprocessing
... -satsolver='k(c_)*' # One Kissat, then search-only (_) CaDiCaL. Always put brackets around the argument of '*'!
... -satsolver='kCLCLcl' # Capital letters indicate using truly incremental SAT solving for incremental jobs
... -satsolver='l+(c!){37}' # One Lingeling configured for satisfiable instances (+), then 37 proof-producing (!) CaDiCaLs, repeat
... -satsolver='(c!){37}k+((c!){37}l+)*' # As above, but replacing the 1st Lingeling with Kissat configured for SAT (+)
... -satsolver='(c!){37}k+[[c!]{37}l+]w' # Alternative notation with squared brackets and automaton-style "omega" (w) to avoid issues with bash
```
The precise meaning of the suffixes "_" and "+" are defined via the next configuration stage.

### Configuring Individual Solver Options

The second stage of configuration allows to define the precise, by-backend configuration of individual solver options under different flavors and other circumstances. 

Users can supply one or more JSON files. These files are parsed in the order given: First, all files in the directories supplied via `-sat-config-dirs` (comma-separated) are evaluated; then all files supplied via `-sat-config-files` are evaluated. Within a file, rules are evaluated in array order. This allows to layer a "defaults" file and an "overrides" file, for example. 

By default, the value of `-sat-config-dirs` enables a range of configuration files (under `config/sat/base/`) that define the basic sequence of configuration options for each solver backend's default, search-only (_), and satisfiable-focused (+) flavors. Additional diversification can then be added by the user, for example, via `-sat-config-files=config/sat/reduce.json` for varying clause database management parameters.

#### Structure of JSON Files

Each JSON file contains a list of "rules". Each rule says: for solver threads of a given BACKEND which matches a given SELECTOR, apply a given list of SETTINGS.

**Valuable examples** are located at `config/sat/base/native.json` (for cycling over a sequence of configuration options for each solver backend) and `config/sat/reduce.json` (for adding random diversification to some specific numeric options).

Here is the **general structure** of such a JSON:

```json
{
  "rules": [
    {
      "backend": <"Kissat" | "CaDiCaL" | "Lingeling">,
      "selector": <selector>,
      "settings": [ 
        {"type": "set", "key": "some-option", "value": "value"}, 
        {"type": "set", "key": "some-option", "value": "sample R1"}, 
            // - value "sample <dist>" draws a sample from distribution <dist>
            //   (see below at "samplers")
        {"type": "add", "key": "another-option", "value": 42,
          "min": 1, "max": 1000},
            // - optional min/max fields clamp the *final* option value to [min, max]
        {"type": "configure", "key": "configure", "value": "plain"}
      ]
    },
    ...
  ],
  "samplers": [
    {
      "name": "R1",
      "distribution": {
        "type": <"constant" | "uniform" | "normal" | "exponential">
        "params": [0, 10, -100000, 100000]
            // type == constant: [val]
            // type == uniform: [min, max]
            // type == normal: [mean, stddev, min, max]
            // type == exponential: [lambda]
      }
    },
    ...
  ]
}
```
A `<selector>` is one of:
```json
{ "type": "true" }
{ "type": "flavour", "value": <"default" | "sat" | "unsat" | "plain" | "preprocess"> }
{ "type": "random",  "chance": <0..1> }
{ "type": "range",   "from": <int>, "to": <int> }                // inclusive [i,j]
{ "type": "scatter", "start": <int>, "step": <int> }             // {start + x*step : x >= 0}
{ "type": "not",     "selector": <selector> }
{ "type": "and",     "selectors": [ <selector>, ... ] }
{ "type": "or",      "selectors": [ <selector>, ... ] }
```
The integers in the "range" and "scatter" selectors reference the solver's global ID within its backend, e.g., 7 if it is the seventh instantiated solver thread of its backend type (e.g., "Kissat") in its distributed job.
A setting value is either a string or a number (integer or floating point).


## Extras

### UNSAT Core Extraction

In **monolithic proof production mode**, it is possible to produce an UNSAT core, i.e., a subset of the formula's clauses that is UNSAT.

Use the option `-core=path/to/out.cnf`.

This procedure can be iterated to successively refine the UNSAT core. **Example:**

```bash
touch out.cnf ; f=instances/r3unsat_300.cnf ; cp $f in.cnf
for i in {1..10}; do  # do 10 times
  rm out.cnf
  echo -ne $(head -1 in.cnf) "=> ... "
  scripts/run/mallob_local.sh $(config/presets/monolithic-proof) -mono=in.cnf -proof=/dev/null -core=out.cnf -q > /dev/null
  echo "=>" $(head -1 out.cnf)
  cp out.cnf in.cnf
done
```

### Model Enumeration

Mallob supports a limited kind of model enumeration that may prove useful and sufficient in certain use cases where not one but several models are desired.

Use `-enum=1` and, optionally, `-enum-timelimit=<limit-secs> -enum-limit=<limit-models>` to limit the enumeration procedure after an initial model was found.

**Examples:**
```bash
# Enumerate solutions indefinitely.
# Use incremental CaDiCaLs, each of which tightly loops to find different models, with thread-local blocking clauses only.
scripts/run/mallob_local.sh -mono=instances/r3sat_300.cnf -enum=1 -satsolver=C -s2f=$solpath
# Enumerate solutions for up to 10s (and stopping after 100 have been found).
# Use non-incremental Kissats, restarting them as needed with all process-local blocking clauses thus far
# and also with all unit clauses shared thus far (disable with -enum-feed-units=0).
scripts/run/mallob_local.sh -mono=instances/r3sat_300.cnf -enum=1 -satsolver=k -s2f=$solpath -enum-timelim=10 -enum-limit=100
```

In the above examples, solutions are written to `${solpath}.#<jobid>.enum.*`, where `<jobid>` is a number that increases with each run of Mallob.
The models are written in a compressed string representation (one hexadecimal digit for each block of four variables).
To decompress and de-duplicate them into a single sequence of unique textual models (`v 1 -2 3 4 -5 ... 0`), use the script `scripts/run/decompress_models.sh`. You can apply it to all solution files of the most recent run simultaneously like this:

```bash
scripts/run/decompress_models.sh $solpath.$(ls -t $solpath.#*|grep -oE "#[0-9]+"|head -1).enum.*
```

**Caveats:**
* The number of distinct solutions found is **over-approximated** by Mallob: Duplicate solutions are only accounted for at a process-local level but not at the distributed level. As such, you may obtain fewer distinct models than expected when running several processes.
* The procedure is not designed to perform exhaustive model enumeration. Since each solver unit (thread or process) only accumulates models at a local level, a single solver unit has to find all of the formula's models (and then prove UNSAT) for the procedure to terminate. Moreover, the worst-case number of clauses (each of size `|V|`) that need to be added is exponential.
* At a programmatic level, only a single model is reported by the distributed SAT task even if many models are enumerated. That said, all of the globally found models are written to files (see above).
