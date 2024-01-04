#!/bin/bash

# Dawn 2022-10-03:
# To postprocess the files, assuming we have original.cnf (original problem), preprocessed.cnf, preprocessed.lrat, id.map, and solved.lrat (combined result of solving):
# 1. renumber-proofs --adjust-file id.map preprocessed.cnf p.lrat solved.lrat
# 2. cat preprocessed.lrat p.lrat > final.lrat
# The first step renumbers the clauses according to the mapping in id.map, as well as renumbering them to be sequential after the clauses produced by the preprocessing.  The second step combines the two proofs into one that can be checked.  If solved.lrat is binary, you need to add the --binary flag in step 1 and step 1.5 needs to compress preprocessed.lrat before combining them. 

# 1 (outputs "p.lrat")
/renumber-proofs --binary --adjust-file /logs/preprocessor/id_map /logs/processes/input_units_removed.cnf \
  /logs/preprocessor/p.lrat /logs/processes/combined.lrat
# 1.5 is done in proof_compose.sh
# 2
cat /logs/preprocessor/preprocessing-compressed.lrat /logs/preprocessor/p.lrat > /logs/processes/renumbered.lrat
