
# Execution Pipeline

## Single-machine Setup, Sequential Assembly

1. Run Mallob WITHOUT distributed assembly
    - Original CNF input is a parameter
    - Marijn's preprocessor (see `preprocess_cnf.sh`) is called from within Mallob
        - Preprocessed CNF is written to `/logs/processes/input_units_removed.cnf`
        - LRAT proof for preprocessing is written to `/logs/preprocessor/preprocessing.lrat`
        - Clause ID map file from preprocessing is written to `/logs/preprocessor/id_map`
    - Preprocessed problem is being solved
    - Partial proofs are written to `/logs/processes/proof#1/proof.*.lrat`
2. Run sequential proof combination on the preprocessed file and the partial proofs (see `proof_compose.sh`)
    - Combined proof is written to `/logs/processes/combined.lrat`
    - An unpruned version of the proof is written to `/logs/processes/drat.lrat` (still in LRAT format!)
    - The LRAT proof of the preprocessing is compressed here (since it is needed for LRAT _and_ DRAT production), written to `/logs/preprocessor/preprocessing-compressed.lrat`
3. Run LRAT proof renumbering (see `proof_renumber.sh`)
    - Renumbered, final proof is written to `/logs/processes/renumbered.lrat`
4. Output proof line counts (see `proof_line_count.sh`)
5. Check produced LRAT proof with `lrat-check`
6. Compose DRAT proof (see `drat_compose.sh`)
    - "Dratifies" preprocessing proof and unpruned combined proof
    - Concatenates them into `/logs/processes/final.drat`
7. Check produced DRAT proof with `drat-trim`

### TODO

* Test
* Parallelize steps 3-5 and 6-7.
* Handle return codes of these steps properly: DRAT failing should not interrupt the other one and vice versa
* Inject timings into the scripts which can then be output
    - In proof_compose.sh there is the problem that the combined LRAT proof and the unpruned "DRAT-ready" proof are output in the same command. If outputting the unpruned proof adds significant overhead, we may want to separate this from each another, adding more redundant work, for proper timings.
* Branch off a version which does not use binary proofs [if we actually want to do this]

## Single-machine Setup, Distributed Assembly

TODO

## Distributed Setup

TODO
