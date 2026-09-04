
# SATWITHPRE Application Engine

In the context of our [SAT'25 publication](https://drops.dagstuhl.de/entities/document/10.4230/LIPIcs.SAT.2025.27) and subsequent [SAT Competition '26 submission](https://satres.kikit.kit.edu/papers/2026-mallob-cascading.pdf), we developed an alternative setup for distributed SAT solving where a number of (pre-)processing actors can interact in a DAG-like pipeline. In this _Cascading Preprocessing_, preprocessors can be chained together - both sequential ones (like Satsuma or Kissat) and parallel ones (like MallobSweep) - and their results can be fed into parallel solvers, running in parallel or displacing each another.

Compile Mallob with `-DMALLOB_APP_SATWITHPRE=1` and then set `-mono-app=SATWITHPRE` to use this setup. Readily configured presets are available via `config/presets/satcomp26-quick*`.

## Configuration

Use the option `-preprocess-config=path/to/config.json`, where the supplied JSON file describes the DAG of actors, how they are configured and how they interact. As an example, consult the JSON files `config/satwithpre/actors_*.json`. Simply put, such a JSON file is a sequence of actor specifications, where each actor has the following fields:

* "id": A freely choosable name for the sake of referencing the actor and representing it in logs.
* "type": One of the supported actor types, see the following list. Some actors are "sink nodes" since they produce no simplified formula but only a final result.
    - "MALLOBSAT" (distributed, sink): MallobSat solving engine
    - "MALLOBSWEEP" (distributed): MallobSweep equivalence sweeping engine
    - "SATSUMA_EXT" (sequential): Satsuma symmetry breaker, called as an external process
    - "SATSUMA_INT" (sequential): Satsuma symmetry breaker, as an integrated library (obsolete / deprecated)
    - "LINGELING" (sequential, sink): Lingeling's non-standard preprocessing methods (Gaussian elimination, Cardinality constraint reasoning)
    - "KISSAT" (sequential): Kissat's full preprocessing arsenal
* "prerequisite": either `null` or the ID of another actor; signifies that this actor starts only after the prerequisite has been executed and uses the result of the prerequisite's preprocessing as applicable. 
* "actorsBeingDisplaced": an array of IDs of other actors which will be replaced (possibly gradually, see `-pb` and `-pef` options) by this actor being executed.
* "onlyStartIfPrerequisiteSimplified": `true` or `false`; if `true`, this actor will only be executed if its **direct** predecessor has reported a simplification of the input. If `false`, this actor will be executed either way.
* "group-id": Group ID for cross-task clause sharing; two processors with the same group ID are allowed to exchange clauses with each another (see `-cjc` option).
* "options": A string of whitespace-separated Mallob program options, overriding the global options for this particular actor.

Reconstruction of a found satisfying variable assignment works by tracing the "winning chain" back to its first actor and successively converting the model back as needed. Compositional proof production for a selected subset of actors is being worked on but not yet merged.
