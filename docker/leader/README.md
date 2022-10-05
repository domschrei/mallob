
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
2. Run sequential proof combination on the preprocessed file and the partial proofs (see `proof_combine.sh`)
    - Combined proof is written to `/logs/processes/combined.lrat`
3. Run proof renumbering (see `proof_renumber.sh`)
    - Renumbered, final proof is written to `/logs/processes/renumbered.lrat`
4. Output proof line counts (see `proof_line_count.sh`)
5. Check produced LRAT proof
6. (**TODO**) Extract DRAT proof from LRAT proof, check DRAT proof
    - In parallel with step 5 or (ideally) steps 2-5

## Single-machine Setup, Distributed Assembly

TODO

## Distributed Setup

TODO
