
# Execution Pipeline

## Before you build + run

* Edit the variable `pipeline_mode` to the desired mode of execution.
* The removal of log directories in `cleanup` is commented out as of now for debugging. Re-add the command as necessary.

## Single-machine Setup, Sequential Assembly (+ DRAT)

This appears to be our most complex setup, so let's start with this one.

### Pipeline

1. Run **Mallob** without distributed assembly
    - Original CNF input is a parameter
    - Marijn's preprocessor (see `preprocess_cnf.sh`) is called from within Mallob
        - Preprocessed CNF is written to `/logs/processes/input_units_removed.cnf`
        - LRAT proof for preprocessing is written to `/logs/preprocessor/preprocessing.lrat`
        - Clause ID map file from preprocessing is written to `/logs/preprocessor/id_map`
    - Preprocessed problem is being solved
    - Partial proofs are written to `/logs/processes/proof#1/proof.*.lrat`
2. **Compress preprocessing proof** (see `compress_preprocessing_proof.sh`)
    - Necessary for later stages, both for LRAT and for DRAT (and also for distributed assembly), therefore in a separate script
    - Outputs compressed preprocessing proof to `/logs/preprocessor/preprocessing-compressed.lrat`
3. Run **sequential proof combination** on the preprocessed file and the partial proofs (see `proof_compose.sh`)
    - Combined proof is written to `/logs/processes/combined.lrat`
    - An unpruned version of the proof is written to `/logs/processes/drat.lrat` (still in LRAT format!)
4. Run **LRAT proof renumbering** (see `proof_renumber.sh`)
    - Renumbered, final proof is written to `/logs/processes/renumbered.lrat`
5. **Output proof line counts** (see `proof_line_count.sh`)
6. **Check LRAT proof** with `lrat-check` (see `proof_check.sh`)
    - This decompresses the compressed proof first! Otherwise lrat-check doesn't seem to work.
7. **Compose DRAT proof** (see `drat_compose.sh`)
    - "Dratifies" preprocessing proof and unpruned combined proof
    - Concatenates them into `/logs/processes/final.drat`
8. **Check DRAT proof** with `drat-trim`
    - dratify outputs text files, so no decompression is needed here

### TODO

* On r3unsat_200.cnf (`competition/test.cnf` in the container) `lrat-check` fails on the LRAT proof with the original input CNF. This seems to be an issue tied to the preprocessing and renumbering stuff - the combined (not renumbered) proof with the preprocessed input CNF is verified successfully. The DRAT proof seems to be correct as well. 
* Parallelize steps 4-6 and steps 7-8.
* Inject proper timings into the scripts which can then be output
    - In proof_compose.sh, the combined LRAT proof and the unpruned "DRAT-ready" proof are output in the same command. The compose executable outputs the time needed to build the unpruned proof though.
* Branch off a version which does not use binary proofs [if we actually want to do this]
* Many file paths are now hardcoded in the many different scripts. -> After the pipeline is fixed, a refactoring putting all these paths into a single place would be nice.

## Single-machine Setup, Distributed Assembly

### Pipeline

1. Run **Mallob** **with** distributed assembly
    - Original CNF input is a parameter
    - Marijn's preprocessor (see `preprocess_cnf.sh`) is called from within Mallob
        - Preprocessed CNF is written to `/logs/processes/input_units_removed.cnf`
        - LRAT proof for preprocessing is written to `/logs/preprocessor/preprocessing.lrat`
        - Clause ID map file from preprocessing is written to `/logs/preprocessor/id_map`
    - Preprocessed problem is being solved
    - Combined proof is written to `/logs/processes/combined.lrat`
2. **Compress preprocessing proof** (see `compress_preprocessing_proof.sh`)
    - Necessary for later stages, both for LRAT and for DRAT, therefore in a separate script
    - Outputs compressed preprocessing proof to `/logs/preprocessor/preprocessing-compressed.lrat`
3. Run **LRAT proof renumbering** (see `proof_renumber.sh`)
    - Renumbered, final proof is written to `/logs/processes/renumbered.lrat`
4. **Output proof line counts** (see `proof_line_count.sh`)
5. **Check LRAT proof** with `lrat-check` (see `proof_check.sh`)
    - This decompresses the compressed proof first! Otherwise lrat-check doesn't seem to work.
6. **Compose DRAT proof** (see `drat_compose.sh`)
    - "Dratifies" preprocessing proof and unpruned combined proof
    - Concatenates them into `/logs/processes/final.drat`
7. **Check DRAT proof** with `drat-trim`
    - dratify outputs text files, so no decompression is needed here

### TODO

* Test
* Adjust proof_line_count to work with the parallel proof combination as well

## Distributed Setup

### Pipeline

Exactly the same as in the Single-machine Setup, Distributed Assembly

### TODO

* Test
