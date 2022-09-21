#!/bin/bash

echo ">>>>>Proof Sizes<<<<<" 1>&2
grep --invert d /logs/processes/._____combined_proof_file.frat | wc -l 1>&2
grep --invert d /logs/processes/combined.lrat | wc -l 1>&2
