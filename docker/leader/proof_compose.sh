#!/bin/bash

# collect all partial proofs with have been written
partialproofs="$(echo /logs/processes/proof#1/proof.*.lrat)"
/compose-proofs --loose --keep-temps $* $partialproofs 1>&2 
