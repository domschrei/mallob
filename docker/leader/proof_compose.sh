#!/bin/bash

# collect all partial proofs with have been written
partialproofs="$(echo /logs/processes/proof#1/proof.*.lrat)"

# this also outputs the unpruned DRAT proof.
/compose-proofs --loose --keep-temps --write-unpruned /logs/processes/drat.lrat --binary $* $partialproofs 1>&2 

# this a necessary step for the production of both LRAT and DRAT proofs
/compress /logs/preprocessor/preprocessing.lrat /logs/preprocessor/preprocessing-compressed.lrat
