
# SATWITHPRE Application Engine

This augmented engine for distributed SAT solving orchestrates a number of (pre-)processing actors that interact in a DAG-like pipeline. In this _Cascading Preprocessing_, preprocessors can be chained together - both sequential ones (like Satsuma or Kissat) and parallel ones (like MallobSweep) - and their results can be fed into parallel solvers, running in parallel or displacing each another.

Compile Mallob with `-DMALLOB_APP_SATWITHPRE=1` and then set `-mono-app=SATWITHPRE` to use this setup. A well performing example **preset** is available via `config/presets/satcomp26-quick`.

## Configuration

Use the option `-preprocess-config=path/to/config.json`, where the supplied JSON file describes the DAG of actors, how they are configured and how they interact.
An **example** is provided further below.

Generally speaking, such a JSON file is a sequence of actor specifications, where each actor has the following fields:

* "id": A freely choosable name for the sake of referencing the actor and representing it in logs.
* "type": One of the supported actor types, see the following list. Some actors are "sink nodes" since they produce no simplified formula but only a final result.
    - "MALLOBSAT" (distributed, sink): MallobSat solving engine
    - "MALLOBSWEEP" (distributed): MallobSweep equivalence sweeping engine
    - "SATSUMA_EXT" (sequential): Satsuma symmetry breaker, called as an external process
    - "SATSUMA_INT" (sequential, deprecated): Satsuma symmetry breaker, as an integrated library
    - "LINGELING" (sequential, sink): Lingeling's non-standard preprocessing methods (Gaussian elimination, Cardinality constraint reasoning)
    - "KISSAT" (sequential): Kissat's full preprocessing arsenal
* "prerequisite": either `null` or the ID of another actor; signifies that this actor starts only after the prerequisite has been executed and uses the result of the prerequisite's preprocessing as applicable. 
* "actorsBeingDisplaced": an array of IDs of other actors which will be replaced (possibly gradually, see `-pb` and `-pef` options) by this actor being executed.
* "onlyStartIfPrerequisiteSimplified": `true` or `false`; if `true`, this actor will only be executed if its **direct** predecessor has reported a simplification of the input. If `false`, this actor will be executed either way.
* "group-id": Group ID for cross-task clause sharing; two processors with the same group ID are allowed to exchange clauses with each another (see `-cjc` option).
* "options": A string of whitespace-separated Mallob program options, overriding the global options for this particular actor.

### Example

This example, from `config/satwithpre/actors_M_SM_SKM.json`, configures a generally well performing set of three chains of actors:

* MallobSat (M / "mallobsat-plain"),
* Satsuma followed by MallobSat (SM / "satsuma" -> "mallobsat-on-satsuma"), and
* Satsuma followed by Kissat followed by MallobSat (SKM / "satsuma" -> "kissat-on-satsuma" -> "mallobsat-on-satsuma-kissat").

The "more preprocessed" MallobSat tasks are configured to displace the "less preprocessed" MallobSat tasks.
Note how "mallobsat-on-satsuma" and "kissat-on-satsuma" make use of the same Satsuma incarnation as their shared prerequisite. Also note that the "group-id" and "options" fields explained above are optional and not used here.

```json
[
    {
        "id": "mallobsat-plain",
        "type": "MALLOBSAT",
        "prerequisite": null,
        "actorsBeingDisplaced": [],
        "onlyStartIfPrerequisiteSimplified": false
    },
    {
        "id": "satsuma",
        "type": "SATSUMA_EXT",
        "prerequisite": null,
        "actorsBeingDisplaced": [],
        "onlyStartIfPrerequisiteSimplified": false
    },
    {
        "id": "mallobsat-on-satsuma",
        "type": "MALLOBSAT",
        "prerequisite": "satsuma",
        "actorsBeingDisplaced": ["mallobsat-plain"],
        "onlyStartIfPrerequisiteSimplified": true
    },
    {
        "id": "kissat-on-satsuma",
        "type": "KISSAT",
        "prerequisite": "satsuma",
        "actorsBeingDisplaced": [],
        "onlyStartIfPrerequisiteSimplified": false
    },
    {
        "id": "mallobsat-on-satsuma-kissat",
        "type": "MALLOBSAT",
        "prerequisite": "kissat-on-satsuma",
        "actorsBeingDisplaced": ["mallobsat-plain", "mallobsat-on-satsuma"],
        "onlyStartIfPrerequisiteSimplified": true
    }
]
```

## Models and Proofs

Reconstruction of a found satisfying variable assignment works by tracing the "winning chain" back to its first actor and successively converting the model back as needed. **Note that this is still WIP for the MallobSweep actor.**

Our cascading preprocessing supports the emission of **compositional proofs** for a certain subset of actors: Kissat (emitting DRUP), Satsuma (emitting DSR), and MallobSat with CaDiCaL as a backend (emitting PalRUP).
Set the option `-prepro-proofs=1 -proof-dir=path/to/proof/output/` to enable proof production. Actors incapable of producing proofs will not report a found UNSAT result to the main solving procedure but can still be employed to boost the search for a satisfying assignment.

Proof checking is performed with the `chaincheck` utility, which integrates and orchestrates important proof checkers and ensures that the "winning chain" leading to unsatisfiability is in fact a coherent chain of arguments.
For convenience, `chaincheck` is an optional part of Mallob's build dependencies, so it can be fetched and built directly by setting the build option `-DMALLOB_BUILD_CHAINCHECK=1` and is then available as an executable at `build/chaincheck`.
You can then call `build/chaincheck path/to/formula.cnf path/to/proof/output/` to check the compositional proof.

Note that, as of yet, `chaincheck` only supports shared-memory checking of PalRUP proofs. If a PalRUP proof is distributed across many machines and/or individual fragments are only accessible from one specific machine, `chaincheck` may fail to check this proof stage. You can however run PalRUP-check yourself in the appropriate distributed configuration, relative to the formula the respective actor used as an input, and as long as `chaincheck` gave a positive response to all other checks, a successful PalRUP-check procedure does confirm the correctness of the result.


## References

**References:**
* [SAT'25 publication](https://drops.dagstuhl.de/entities/document/10.4230/LIPIcs.SAT.2025.27)
* [SAT Competition '26 submission](https://satres.kikit.kit.edu/papers/2026-mallob-cascading.pdf)
